/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTFacts.h"
#include "AOTGrowthOfWork.h"
#include "AOTIRParser.h"
#include "AOTLinkTables.h"
#include "AOTNativeTest.h"
#include "AOTPlan.h"
#include "AOTProgramFile.h"
#include "AOTScratchRealm.h"
#include "AOTShellProgramTestSupport.h"
#include "AOTTestHarness.h"
#include "JSGlobalObject.h"
#include "VM.h"
#include <wtf/text/StringBuilder.h>
#include <wtf/text/StringToIntegerConversion.h>

namespace JSC { namespace AOT {

std::span<const NativeTest> growthOfWorkTests();

using namespace Typed;

static String classWithManyMethodsForGrowthTests(unsigned numberOfMethods)
{
    StringBuilder text;
    text.append("layout L0 kind=Instance name=\"Wide\" module=m0 constructor=f1\n"_s);
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\" body=f0 namespaceLayout=L(m0)\nentry module=m0\n"_s);
    text.append("function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = NewClosure function=f1\n    v1 = NewClass v0 layout=L0\n"_s);
    unsigned value = 2;
    for (unsigned method = 0; method < numberOfMethods; ++method, value += 2) {
        text.append("    v"_s, value, " = NewClosure function=f"_s, method + 2, '\n');
        text.append("    DefineMethod v1, v"_s, value, " layout=L0 index="_s, method, " key=\"m"_s, method, "\"\n"_s);
    }
    unsigned classObject = value++;
    unsigned object = value++;
    text.append("    v"_s, classObject, " = FinishClass v1 layout=L0\n    v"_s, object, " = Construct v"_s, classObject, '\n');
    for (unsigned method = 0; method < numberOfMethods; ++method, value += 2) {
        text.append("    v"_s, value, " = PropGet v"_s, object, " key=\"m"_s, method, "\"\n"_s);
        text.append("    v"_s, value + 1, " = Call v"_s, value, ", v"_s, object, " calleeKey=\"m"_s, method, "\"\n"_s);
    }
    text.append("    Return v"_s, object, "\n}\n"_s);
    text.append("function f1 name=\"Wide\" module=m0 kind=BaseConstructor flags=[HasNewTarget, HasExplicitName] thisType={type=anything} results=[{type=object flags=[IsFromCompiler]}] constructedClass=L0 {\n"_s);
    text.append("  b0(v0: anything):\n    v1 = GetConstructPrototype v0 layout=L0\n    v2 = NewObject v1 layout=L0\n    Return v2\n}\n"_s);
    for (unsigned method = 0; method < numberOfMethods; ++method) {
        text.append("function f"_s, method + 2, " name=\"m"_s, method, "\" module=m0 kind=Method flags=[HasExplicitName] thisType={type=anything} results=[{type=anything}] {\n"_s);
        text.append("  b0():\n    v0 = Integer integer=1\n    Return v0\n}\n"_s);
    }
    return text.toString();
}

static void testEachOfManyMembersIsFoundByItsKey()
{
    constexpr uint32_t numberOfKeys = 300000;
    constexpr uint32_t step = 7919;
    runOnBuildThread([&] {
        BuildContext build(makeOptionsOfTestedProgram());
        std::unique_ptr<Program> program = parseProgram(withStandInLibrary(classWithManyMethodsForGrowthTests(1)), build);
        AOT_TEST_CHECK(program);
        LayoutID wide(0u);
        LinkTables tables = conservativeLinkTables(*program);
        tables.layouts[wide.index()].listsEveryMemberAbove = true;
        tables.layouts[wide.index()].namesAreDefinedPastLists = { };

        auto keyOf = [&](uint32_t number) {
            return PropertyKeyID(static_cast<uint32_t>(static_cast<uint64_t>(number) * step % numberOfKeys));
        };
        auto append = [&](MethodKind kind, PropertyKeyID key, FunctionID function) {
            LinkedMember member;
            member.classLayout = wide;
            member.kind = kind;
            member.index = static_cast<uint32_t>(tables.members.size());
            member.key = key;
            member.function = function;
            member.everyWriterIsKnown = true;
            tables.members.append(member);
        };
        for (uint32_t number = 1; number < numberOfKeys; ++number)
            append(number % 3 == 1 ? MethodKind::Getter : MethodKind::Method, keyOf(number), FunctionID(number));
        for (uint32_t number = 1; number < numberOfKeys; ++number) {
            if (number % 3)
                append(number % 3 == 1 ? MethodKind::Setter : MethodKind::Method, keyOf(number), FunctionID(numberOfKeys + number));
        }
        tables.indexMembersByKey(*program);

        Vector<FunctionID, 4> functions;
        for (uint32_t number = 1; number < numberOfKeys; ++number) {
            MemberFound found = tables.findMember(*program, wide, ClassSide::Prototype, keyOf(number));
            functions.shrink(0);
            bool isTold = tables.findFunctionsLaidAbove(*program, wide, keyOf(number), functions);
            switch (number % 3) {
            case 0:
                AOT_TEST_CHECK(found.kind == MemberFound::Kind::Method && found.method == FunctionID(number) && !found.getter && !found.setter);
                AOT_TEST_CHECK(isTold && functions.size() == 1 && functions[0] == FunctionID(number));
                break;
            case 1:
                AOT_TEST_CHECK(found.kind == MemberFound::Kind::Accessor && !found.method && found.getter == FunctionID(number) && found.setter == FunctionID(numberOfKeys + number));
                AOT_TEST_CHECK(!isTold);
                break;
            case 2:
                AOT_TEST_CHECK(found.kind == MemberFound::Kind::Unknown);
                AOT_TEST_CHECK(isTold && functions.size() == 2 && functions[0] == FunctionID(number) && functions[1] == FunctionID(numberOfKeys + number));
                break;
            }
            AOT_TEST_CHECK(tables.membersUnder(wide, ClassSide::Static, keyOf(number)).empty());
        }
        AOT_TEST_CHECK(tables.findsNothing(*program, wide, ClassSide::Prototype, keyOf(0), Of::HolderAndAbove).has_value());
    });
}

static std::optional<uint64_t> instructionsOfLookupsFromFootOfRowForGrowthTests(unsigned numberOfClasses)
{
    StringBuilder text;
    text.append("layout L0 kind=Instance name=\"C0\" module=m0\n"_s);
    for (unsigned number = 1; number < numberOfClasses; ++number)
        text.append("layout L"_s, number, " kind=Instance name=\"C"_s, number, "\" module=m0 base=L"_s, number - 1, " supertypes=[L"_s, number - 1, "] flags=[HasHeritage]\n"_s);
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\nentry module=m0\n"_s);
    text.append("function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = Builtin builtin=GlobalThis\n"_s);
    for (unsigned number = 0; number <= numberOfClasses; ++number)
        text.append("    v"_s, number + 1, " = PropGet v0 key=\"member"_s, number, "\"\n"_s);
    text.append("    Return v1\n}\n"_s);

    std::optional<uint64_t> instructions;
    runOnBuildThread([&] {
        BuildContext build(makeOptionsOfTestedProgram());
        std::unique_ptr<Program> program = parseProgram(withStandInLibrary(text.toString()), build);
        AOT_TEST_CHECK(program);
        LinkTables tables = conservativeLinkTables(*program);
        Vector<PropertyKeyID> keys(numberOfClasses + 1, [&](size_t number) {
            StringID name = program->strings.find(encodeProgramString(makeString("member"_s, number)).span());
            return program->types.find(PropertyKey { .kind = PropertyKeyKind::Text, .text = name }).value_or(PropertyKeyID());
        });
        for (uint32_t number = 0; number < numberOfClasses; ++number) {
            tables.layouts[number].listsEveryMemberAbove = true;
            tables.layouts[number].hasConstructorFunctionInRow = false;
            tables.layouts[number].namesAreDefinedPastLists = { };
            LinkedMember member;
            member.classLayout = LayoutID(number);
            member.key = keys[number];
            member.function = FunctionID(number + 1);
            member.everyWriterIsKnown = true;
            tables.members.append(member);
        }
        tables.indexMembersByKey(*program);
        LayoutID foot(numberOfClasses - 1);
        instructions = instructionsOfCurrentThreadIn([&] {
            for (uint32_t number = 0; number < numberOfClasses; ++number) {
                MemberFound found = tables.findMember(*program, foot, ClassSide::Prototype, keys[number]);
                AOT_TEST_CHECK(found.kind == MemberFound::Kind::Method && found.method == FunctionID(number + 1));
                bool isDeclaredHere = number + 1 == numberOfClasses;
                AOT_TEST_CHECK(tables.findMember(*program, LayoutID(number), ClassSide::Prototype, keys[numberOfClasses - 1]).kind == (isDeclaredHere ? MemberFound::Kind::Method : MemberFound::Kind::Unknown));
                AOT_TEST_CHECK(tables.findsNothing(*program, LayoutID(number), ClassSide::Prototype, keys[numberOfClasses - 1], Of::HolderAndAbove).has_value() == !isDeclaredHere);
            }
            AOT_TEST_CHECK(tables.findsNothing(*program, foot, ClassSide::Prototype, keys[numberOfClasses], Of::HolderAndAbove).has_value());
        });
    });
    return instructions;
}

static void testMembersOfClassesFarAboveAreFoundByTheirKeys()
{
    constexpr unsigned numberOfClasses = 1000;
    InstructionCounts::begin();
    std::optional<uint64_t> smaller = instructionsOfLookupsFromFootOfRowForGrowthTests(numberOfClasses);
    std::optional<uint64_t> large = instructionsOfLookupsFromFootOfRowForGrowthTests(4 * numberOfClasses);
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

static std::optional<uint64_t> instructionsOfAskingEachClassObjectOfRowForGrowthTests(unsigned numberOfClasses)
{
    StringBuilder text;
    text.append("layout L0 kind=Instance name=\"C0\" module=m0\n"_s);
    for (unsigned number = 1; number < numberOfClasses; ++number)
        text.append("layout L"_s, number, " kind=Instance name=\"C"_s, number, "\" module=m0 base=L"_s, number - 1, " supertypes=[L"_s, number - 1, "] flags=[HasHeritage]\n"_s);
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\nentry module=m0\n"_s);
    text.append("function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = Builtin builtin=GlobalThis\n    v1 = PropGet v0 key=\"asked\"\n    Return v1\n}\n"_s);

    std::optional<uint64_t> instructions;
    runOnBuildThread([&] {
        BuildContext build(makeOptionsOfTestedProgram());
        std::unique_ptr<Program> program = parseProgram(withStandInLibrary(text.toString()), build);
        AOT_TEST_CHECK(program);
        LinkTables tables = conservativeLinkTables(*program);
        StringID name = program->strings.find(encodeProgramString(String("asked"_s)).span());
        PropertyKeyID asked = program->types.find(PropertyKey { .kind = PropertyKeyKind::Text, .text = name }).value_or(PropertyKeyID());
        AOT_TEST_CHECK(asked);
        for (uint32_t number = 0; number < numberOfClasses; ++number) {
            tables.layouts[number].listsEveryStaticMember = true;
            tables.layouts[number].namesAreDefinedPastLists = { };
            tables.layouts[number].hasConstructorFunctionInRow = false;
        }
        tables.indexMembersByKey(*program);
        instructions = instructionsOfCurrentThreadIn([&] {
            for (uint32_t number = 0; number < numberOfClasses; ++number)
                AOT_TEST_CHECK(tables.findsNothing(*program, LayoutID(number), ClassSide::Static, asked, Of::HolderAndAbove).has_value());
        });
    });
    return instructions;
}

static void testAskingEachClassObjectOfRowWhatLookupDoesNotFindGrowsAsRowDoes()
{
    constexpr unsigned numberOfClasses = 1000;
    InstructionCounts::begin();
    std::optional<uint64_t> smaller = instructionsOfAskingEachClassObjectOfRowForGrowthTests(numberOfClasses);
    std::optional<uint64_t> large = instructionsOfAskingEachClassObjectOfRowForGrowthTests(4 * numberOfClasses);
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

static void testMemberAboveLongRowIsTheOneThatWalkUpRowFinds()
{
    constexpr unsigned lengthOfRow = 24;
    constexpr unsigned numberOfSiblings = 20;
    constexpr unsigned numberOfKeys = 6;
    Vector<std::optional<unsigned>> bases;
    for (unsigned number = 0; number < lengthOfRow; ++number)
        bases.append(number ? std::optional<unsigned>(number - 1) : std::nullopt);
    for (unsigned number = 0; number < numberOfSiblings; ++number)
        bases.append(1 + number % 12);
    unsigned footOfSide = static_cast<unsigned>(bases.size()) + lengthOfRow - 1;
    for (unsigned number = 0; number < lengthOfRow; ++number)
        bases.append(number ? static_cast<unsigned>(bases.size()) - 1 : 4);

    StringBuilder text;
    for (unsigned number = 0; number < bases.size(); ++number) {
        text.append("layout L"_s, number, " kind=Instance name=\"C"_s, number, "\" module=m0"_s);
        if (bases[number])
            text.append(" base=L"_s, *bases[number], " supertypes=[L"_s, *bases[number], "] flags=[HasHeritage]"_s);
        text.append('\n');
    }
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\nentry module=m0\n"_s);
    text.append("function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = Builtin builtin=GlobalThis\n"_s);
    for (unsigned number = 0; number < numberOfKeys; ++number)
        text.append("    v"_s, number + 1, " = PropGet v0 key=\"member"_s, number, "\"\n"_s);
    text.append("    Return v1\n}\n"_s);

    runOnBuildThread([&] {
        BuildContext build(makeOptionsOfTestedProgram());
        std::unique_ptr<Program> program = parseProgram(withStandInLibrary(text.toString()), build);
        AOT_TEST_CHECK(program);
        LinkTables tables = conservativeLinkTables(*program);
        Vector<PropertyKeyID> keys(numberOfKeys, [&](size_t number) {
            StringID name = program->strings.find(encodeProgramString(makeString("member"_s, number)).span());
            return program->types.find(PropertyKey { .kind = PropertyKeyKind::Text, .text = name }).value_or(PropertyKeyID());
        });
        for (uint32_t number = 0; number < bases.size(); ++number) {
            tables.layouts[number].listsEveryMemberAbove = true;
            tables.layouts[number].listsEveryStaticMember = true;
            tables.layouts[number].hasConstructorFunctionInRow = false;
            tables.layouts[number].namesAreDefinedPastLists = { };
        }
        auto append = [&](unsigned classLayout, ClassSide side, MethodKind kind, unsigned key) {
            LinkedMember member;
            member.classLayout = LayoutID(classLayout);
            member.side = side;
            member.kind = kind;
            member.key = keys[key];
            member.function = FunctionID(static_cast<uint32_t>(tables.members.size()) + 1);
            member.everyWriterIsKnown = true;
            tables.members.append(member);
        };
        for (ClassSide side : { ClassSide::Prototype, ClassSide::Static }) {
            append(0, side, MethodKind::Method, 0);
            append(9, side, MethodKind::Method, 0);
            append(0, side, MethodKind::Method, 1);
            for (unsigned number = 0; number < numberOfSiblings; ++number)
                append(lengthOfRow + number, side, MethodKind::Method, 1);
            append(footOfSide - 3, side, MethodKind::Method, 1);
            append(0, side, MethodKind::Getter, 2);
            append(0, side, MethodKind::Setter, 2);
            append(9, side, MethodKind::Getter, 2);
            append(lengthOfRow - 1, side, MethodKind::Method, 3);
            append(footOfSide, side, MethodKind::Method, 3);
        }
        append(2, ClassSide::Prototype, MethodKind::Method, 4);
        append(20, ClassSide::Static, MethodKind::Method, 4);
        std::ranges::sort(tables.members, { }, [](const LinkedMember& member) {
            return std::tuple { member.classLayout, member.side, member.function };
        });
        tables.indexMembersByKey(*program);

        auto walkUpRow = [&](LayoutID layout, ClassSide side, PropertyKeyID key) {
            MemberFound found;
            for (LayoutID current = layout; current && found.kind == MemberFound::Kind::Unknown; current = program->at(current).base) {
                for (const LinkedMember& member : tables.members) {
                    if (member.classLayout != current || member.side != side || member.key != key)
                        continue;
                    (member.kind == MethodKind::Method ? found.method : member.kind == MethodKind::Getter ? found.getter : found.setter) = member.function;
                    found.kind = member.kind == MethodKind::Method ? MemberFound::Kind::Method : MemberFound::Kind::Accessor;
                }
            }
            return found;
        };
        for (uint32_t number = 0; number < bases.size(); ++number) {
            for (ClassSide side : { ClassSide::Prototype, ClassSide::Static }) {
                for (PropertyKeyID key : keys) {
                    MemberFound found = tables.findMember(*program, LayoutID(number), side, key);
                    MemberFound expected = walkUpRow(LayoutID(number), side, key);
                    AOT_TEST_CHECK(found.kind == expected.kind && found.method == expected.method && found.getter == expected.getter && found.setter == expected.setter);
                    AOT_TEST_CHECK(tables.findsNothing(*program, LayoutID(number), side, key, Of::HolderAndAbove).has_value() == (expected.kind == MemberFound::Kind::Unknown));
                }
            }
        }
    });
}

static void testEachQuestionInsideWhatLookupDidNotFindAnswersForItself()
{
    StringBuilder text;
    text.append("layout L0 kind=Instance name=\"Root\" module=m0\n"_s);
    text.append("layout L1 kind=Instance name=\"Base\" module=m0 base=L0 supertypes=[L0] flags=[HasHeritage]\n"_s);
    text.append("layout L2 kind=Instance name=\"Heir\" module=m0 base=L1 fields=[{key=\"own\" declared={type=anything}}] supertypes=[L1] flags=[HasHeritage]\n"_s);
    text.append("layout L3 kind=Instance name=\"Maker\" module=m0\n"_s);
    text.append("layout L4 kind=Instance name=\"Below\" module=m0 base=L3 supertypes=[L3] flags=[HasHeritage]\n"_s);
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\nentry module=m0\n"_s);
    text.append("function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = Builtin builtin=GlobalThis\n"_s);
    text.append("    v1 = PropGet v0 key=\"asked\"\n    v2 = PropGet v0 key=\"own\"\n    v3 = PropGet v0 key=\"constructor\"\n    v4 = PropGet v0 key=\"name\"\n    v5 = PropGet v0 key=\"arguments\"\n    Return v1\n}\n"_s);

    runOnBuildThread([&] {
        BuildContext build(makeOptionsOfTestedProgram());
        std::unique_ptr<Program> program = parseProgram(withStandInLibrary(text.toString()), build);
        AOT_TEST_CHECK(program);
        LayoutID root(0u);
        LayoutID base(1u);
        LayoutID heir(2u);
        LayoutID maker(3u);
        LayoutID below(4u);
        FunctionID functionOfMaker(0u);
        program->layouts[maker.index()].flags.add(LayoutFlags::IsConstructorFunction);
        program->layouts[maker.index()].constructor = functionOfMaker;
        auto keyNamed = [&](ASCIILiteral name) {
            StringID found = program->strings.find(encodeProgramString(String(name)).span());
            return program->types.find(PropertyKey { .kind = PropertyKeyKind::Text, .text = found }).value_or(PropertyKeyID());
        };
        PropertyKeyID asked = keyNamed("asked"_s);
        AOT_TEST_CHECK(asked);

        auto tablesThatSayNoNowhere = [&] {
            LinkTables tables = conservativeLinkTables(*program);
            for (LayoutID layout : { root, base, heir, maker, below }) {
                LinkedLayout& linked = tables.layouts[layout.index()];
                linked.possibleChanges = { };
                linked.listsEveryMemberAbove = true;
                linked.listsEveryStaticMember = true;
                linked.namesAreDefinedPastLists = { };
                linked.hasConstructorFunctionInRow = layout == maker || layout == below;
            }
            tables.functions[functionOfMaker.index()].closuresGetOnlyNamesThatAreListed = true;
            tables.indexMembersByKey(*program);
            return tables;
        };
        auto finds = [&](const LinkTables& tables, LayoutID layout, ClassSide side, Of of, PropertyKeyID key) {
            return tables.findsNothing(*program, layout, side, key, of).has_value();
        };
        auto findsBehind = [&](const auto& change, LayoutID layout, ClassSide side, Of of) {
            LinkTables tables = tablesThatSayNoNowhere();
            change(tables);
            tables.indexMembersByKey(*program);
            return finds(tables, layout, side, of, asked);
        };
        auto findsBesideStaticFieldOf = [&](LayoutID holder, LayoutID layout, ClassSide side, Of of) {
            Layout& written = program->layouts[holder.index()];
            StaticField field;
            field.key = asked;
            written.staticFields.append(field);
            written.indexFields();
            bool answer = finds(tablesThatSayNoNowhere(), layout, side, of, asked);
            written.staticFields.removeLast();
            written.indexFields();
            return answer;
        };
        auto memberOf = [&](LayoutID classLayout, ClassSide side) {
            return [=](LinkTables& tables) {
                LinkedMember member;
                member.classLayout = classLayout;
                member.side = side;
                member.key = asked;
                member.function = FunctionID(1u);
                member.everyWriterIsKnown = true;
                tables.members.append(member);
            };
        };
        auto nameOnPrototypeOfMaker = [&](PropertyKeyID key) {
            return [=](LinkTables& tables) {
                LinkedNameOnPrototype name;
                name.layout = maker;
                name.key = key;
                tables.namesOnPrototypes.append(name);
            };
        };

        LinkTables clear = tablesThatSayNoNowhere();
        AOT_TEST_CHECK(finds(clear, heir, ClassSide::Prototype, Of::ObjectItself, asked));
        for (ClassSide side : { ClassSide::Prototype, ClassSide::Static }) {
            AOT_TEST_CHECK(finds(clear, heir, side, Of::HolderItself, asked));
            AOT_TEST_CHECK(finds(clear, heir, side, Of::HolderAndAbove, asked));
        }
        AOT_TEST_CHECK(finds(clear, maker, ClassSide::Static, Of::HolderItself, asked));
        AOT_TEST_CHECK(finds(clear, maker, ClassSide::Prototype, Of::HolderItself, asked));
        AOT_TEST_CHECK(finds(clear, below, ClassSide::Prototype, Of::HolderAndAbove, asked));

        AOT_TEST_CHECK(!finds(clear, heir, ClassSide::Prototype, Of::ObjectItself, keyNamed("own"_s)));
        AOT_TEST_CHECK(!findsBehind([&](LinkTables& tables) {
            tables.firstAppendedFieldOfLayout.fill(0, program->layouts.size() + 1);
            for (size_t layout = heir.index() + 1; layout < tables.firstAppendedFieldOfLayout.size(); ++layout)
                tables.firstAppendedFieldOfLayout[layout] = 1;
            tables.appendedKeys.append(asked);
        }, heir, ClassSide::Prototype, Of::ObjectItself));
        AOT_TEST_CHECK(!findsBehind([&](LinkTables& tables) { tables.layouts[heir.index()].possibleChanges = { LayoutChange::GainsProperty }; }, heir, ClassSide::Prototype, Of::ObjectItself));
        AOT_TEST_CHECK(!findsBehind([&](LinkTables& tables) { tables.layouts[heir.index()].possibleChanges = { LayoutChange::GetsAccessor }; }, heir, ClassSide::Prototype, Of::ObjectItself));
        AOT_TEST_CHECK(!findsBehind([&](LinkTables& tables) { tables.layouts[heir.index()].isFilledUnderComputedKeys = true; }, heir, ClassSide::Prototype, Of::ObjectItself));

        for (ClassSide side : { ClassSide::Prototype, ClassSide::Static }) {
            ClassSide otherSide = side == ClassSide::Prototype ? ClassSide::Static : ClassSide::Prototype;
            AOT_TEST_CHECK(!findsBehind(memberOf(heir, side), heir, side, Of::HolderItself));
            AOT_TEST_CHECK(!findsBehind(memberOf(heir, side), heir, side, Of::HolderAndAbove));
            AOT_TEST_CHECK(findsBehind(memberOf(heir, side), heir, otherSide, Of::HolderAndAbove));
            AOT_TEST_CHECK(!findsBehind(memberOf(base, side), heir, side, Of::HolderAndAbove));
            AOT_TEST_CHECK(!findsBehind(memberOf(root, side), heir, side, Of::HolderAndAbove));
            AOT_TEST_CHECK(findsBehind(memberOf(root, side), heir, side, Of::HolderItself));

            auto bitOfHeir = [&](LinkTables& tables) { tables.layouts[heir.index()].namesAreDefinedPastLists[static_cast<size_t>(side)] = true; };
            AOT_TEST_CHECK(!findsBehind(bitOfHeir, heir, side, Of::HolderItself));
            AOT_TEST_CHECK(!findsBehind(bitOfHeir, heir, side, Of::HolderAndAbove));
            AOT_TEST_CHECK(findsBehind(bitOfHeir, heir, otherSide, Of::HolderAndAbove));

            auto listsOfHeirAreNotWhole = [&](LinkTables& tables) { (side == ClassSide::Prototype ? tables.layouts[heir.index()].listsEveryMemberAbove : tables.layouts[heir.index()].listsEveryStaticMember) = false; };
            AOT_TEST_CHECK(!findsBehind(listsOfHeirAreNotWhole, heir, side, Of::HolderAndAbove));
            AOT_TEST_CHECK(findsBehind(listsOfHeirAreNotWhole, heir, otherSide, Of::HolderAndAbove));
        }

        AOT_TEST_CHECK(!findsBesideStaticFieldOf(heir, heir, ClassSide::Static, Of::HolderItself));
        AOT_TEST_CHECK(!findsBesideStaticFieldOf(heir, heir, ClassSide::Static, Of::HolderAndAbove));
        AOT_TEST_CHECK(!findsBesideStaticFieldOf(base, heir, ClassSide::Static, Of::HolderAndAbove));
        AOT_TEST_CHECK(!findsBesideStaticFieldOf(root, heir, ClassSide::Static, Of::HolderAndAbove));
        AOT_TEST_CHECK(findsBesideStaticFieldOf(root, heir, ClassSide::Static, Of::HolderItself));
        AOT_TEST_CHECK(findsBesideStaticFieldOf(heir, heir, ClassSide::Prototype, Of::HolderAndAbove));

        AOT_TEST_CHECK(!findsBehind([&](LinkTables& tables) { tables.functions[functionOfMaker.index()].closuresGetOnlyNamesThatAreListed = false; }, maker, ClassSide::Static, Of::HolderItself));
        AOT_TEST_CHECK(!findsBehind([&](LinkTables& tables) { tables.namesOnFunctions.append(LinkedNameOnFunction { functionOfMaker, asked, FunctionID() }); }, maker, ClassSide::Static, Of::HolderItself));
        AOT_TEST_CHECK(!finds(clear, maker, ClassSide::Static, Of::HolderItself, keyNamed("arguments"_s)));
        AOT_TEST_CHECK(!finds(clear, below, ClassSide::Static, Of::HolderAndAbove, asked));

        AOT_TEST_CHECK(!findsBehind(nameOnPrototypeOfMaker(asked), maker, ClassSide::Prototype, Of::HolderItself));
        AOT_TEST_CHECK(!findsBehind(nameOnPrototypeOfMaker(asked), below, ClassSide::Prototype, Of::HolderAndAbove));
        AOT_TEST_CHECK(!findsBehind(nameOnPrototypeOfMaker({ }), below, ClassSide::Prototype, Of::HolderAndAbove));
        AOT_TEST_CHECK(findsBehind(nameOnPrototypeOfMaker(asked), below, ClassSide::Prototype, Of::HolderItself));

        AOT_TEST_CHECK(!finds(clear, heir, ClassSide::Prototype, Of::HolderItself, keyNamed("constructor"_s)));
        AOT_TEST_CHECK(!finds(clear, heir, ClassSide::Static, Of::HolderItself, keyNamed("name"_s)));
        AOT_TEST_CHECK(!finds(clear, heir, ClassSide::Prototype, Of::HolderItself, PropertyKeyID()));
    });
}

static void testEachOfManyEffectsIsKeptOnceAndStaysWhereItLies()
{
    constexpr uint32_t numberOfEffects = 100000;
    auto effectsOf = [](uint32_t number) {
        Effects effects = Effects::none();
        effects.reads.add(AbstractHeap::ObjectFields, uint64_t { number } + 1);
        effects.writes.add(AbstractHeap::ModuleVariables, uint64_t { number } * 7919 + 1);
        effects.mayCollect = !!(number % 2);
        return effects;
    };
    DistinctEffects distinct;
    Vector<const Effects*> kept(numberOfEffects);
    for (unsigned round = 0; round < 2; ++round) {
        for (uint32_t number = 0; number < numberOfEffects; ++number)
            kept[number] = distinct.keep(effectsOf(number));
    }
    AOT_TEST_CHECK(*distinct.keep(Effects::unknownCode()) == Effects::unknownCode());

    Vector<Vector<Effects>> blocks;
    blocks.appendVector(distinct.takeBlocks());
    size_t numberInBlocks = 0;
    for (const Vector<Effects>& block : blocks)
        numberInBlocks += block.size();
    AOT_TEST_CHECK(numberInBlocks == numberOfEffects);
    for (uint32_t number = 0; number < numberOfEffects; ++number)
        AOT_TEST_CHECK(*kept[number] == effectsOf(number));
}

static std::optional<uint64_t> instructionsOfPlanForGrowthTests(const String& text)
{
    std::optional<uint64_t> instructions;
    runOnBuildThread([&] {
        BuildContext build(makeOptionsOfTestedProgram());
        std::unique_ptr<Program> parsed = parseProgram(withStandInLibrary(text), build);
        AOT_TEST_CHECK(parsed);
        StagedBuild stages(WTF::move(parsed), build);
        AOT_TEST_CHECK(stages.facts());
        instructions = instructionsOfAllThreadsIn([&] {
            AOT_TEST_CHECK(stages.plan());
        });
    });
    return instructions;
}

static void testPlanGrowsAsMembersOfClassDo()
{
    constexpr unsigned numberOfMethods = 1500;
    InstructionCounts::begin();
    std::optional<uint64_t> smaller = instructionsOfPlanForGrowthTests(classWithManyMethodsForGrowthTests(numberOfMethods));
    std::optional<uint64_t> large = instructionsOfPlanForGrowthTests(classWithManyMethodsForGrowthTests(4 * numberOfMethods));
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

static String rowOfClassesWithFieldEachForGrowthTests(unsigned numberOfClasses)
{
    StringBuilder text;
    text.append("layout L0 kind=Instance name=\"C0\" module=m0 constructor=f1 fields=[{key=\"f0\" declared={type=anything} flags=[IsRequired]}] ownFieldCount=1\n"_s);
    for (unsigned number = 1; number < numberOfClasses; ++number) {
        text.append("layout L"_s, number, " kind=Instance name=\"C"_s, number, "\" module=m0 base=L"_s, number - 1, " constructor=f"_s, number + 1);
        text.append(" fields=[{key=\"f"_s, number, "\" declared={type=anything} flags=[IsRequired]}] supertypes=[L"_s, number - 1, "] flags=[HasHeritage] ownFieldCount=1\n"_s);
    }
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\nentry module=m0\n"_s);
    text.append("function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = Builtin builtin=GlobalThis\n    v1 = PropGet v0 key=\"sink\" [isFreeName]\n"_s);
    text.append("    v2 = NewClosure function=f1\n    v3 = NewClass v2 layout=L0\n    v4 = FinishClass v3 layout=L0\n    v5 = Convert v4 declared={type=f1 flags=[IsFromCompiler]}\n"_s);
    unsigned value = 6;
    for (unsigned number = 1; number < numberOfClasses; ++number, value += 5) {
        text.append("    v"_s, value, " = ResolveHeritage v"_s, value - 1, "\n    v"_s, value + 1, " = NewClosure function=f"_s, number + 1, "\n    v"_s, value + 2, " = NewClass v"_s, value + 1, ", v"_s, value - 1, ", v"_s, value, " layout=L"_s, number, '\n');
        text.append("    v"_s, value + 3, " = FinishClass v"_s, value + 2, " layout=L"_s, number, "\n    v"_s, value + 4, " = Convert v"_s, value + 3, " declared={type=f"_s, number + 1, " flags=[IsFromCompiler]}\n"_s);
    }
    text.append("    v"_s, value, " = Construct v"_s, value - 1, ", -\n    v"_s, value + 1, " = PropGet v"_s, value, " key=\"f0\"\n    v"_s, value + 2, " = Call v1, -, v"_s, value + 1, "\n    Return v"_s, value + 2, "\n}\n"_s);
    text.append("function f1 name=\"C0\" module=m0 kind=BaseConstructor flags=[HasNewTarget] thisType={type=anything} results=[{type=object flags=[IsFromCompiler]}] constructedClass=L0 {\n  b0(v0: anything):\n"_s);
    text.append("    v1 = GetConstructPrototype v0 layout=L0\n    v2 = NewObject v1 layout=L0\n    v3 = Convert v2 declared={type=L0}\n    v4 = Integer integer=1\n    PropSet v3, v4 key=\"f0\"\n    Return v3\n}\n"_s);
    for (unsigned number = 1; number < numberOfClasses; ++number) {
        text.append("function f"_s, number + 1, " name=\"C"_s, number, "\" module=m0 kind=DerivedConstructor flags=[HasNewTarget, TakesCallee] thisType={type=anything} results=[{type=object flags=[IsFromCompiler]}] constructedClass=L"_s, number, " {\n"_s);
        text.append("  b0(v0: function, v1: anything):\n    v2 = GetSuperBase v0\n    v3 = SuperConstruct v2, v1 layout=L"_s, number, "\n    v4 = Convert v3 declared={type=L"_s, number, "}\n    v5 = Integer integer=1\n"_s);
        text.append("    PropSet v4, v5 key=\"f"_s, number, "\"\n    Return v4\n}\n"_s);
    }
    return text.toString();
}

static void testPlanGrowsAsClassesInRowDo()
{
    constexpr unsigned numberOfClasses = 100;
    InstructionCounts::begin();
    std::optional<uint64_t> smaller = instructionsOfPlanForGrowthTests(rowOfClassesWithFieldEachForGrowthTests(numberOfClasses));
    std::optional<uint64_t> large = instructionsOfPlanForGrowthTests(rowOfClassesWithFieldEachForGrowthTests(4 * numberOfClasses));
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

static String functionWithManyParametersForGrowthTests(unsigned numberOfParameters)
{
    StringBuilder text;
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\nentry module=m0\n"_s);
    text.append("function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = Builtin builtin=GlobalThis\n    v1 = PropGet v0 key=\"sink\" [isFreeName]\n"_s);
    text.append("    v2 = NewClosure function=f1\n    v3 = Call v1, -, v2\n    Return v3\n}\n"_s);
    text.append("function f1 name=\"run\" module=m0 length="_s, numberOfParameters, " thisType={type=anything} parameters=["_s);
    for (unsigned parameter = 0; parameter < numberOfParameters; ++parameter)
        text.append(parameter ? ", "_s : ""_s, "{type=anything}"_s);
    text.append("] results=[{type=anything}] {\n  b0("_s);
    for (unsigned parameter = 0; parameter < numberOfParameters; ++parameter)
        text.append(parameter ? ", v"_s : "v"_s, parameter, ": anything"_s);
    text.append("):\n    Return v0\n}\n"_s);
    return text.toString();
}

static void testFunctionWithMoreParametersThanConventionCountsIsRefused()
{
    for (auto [numberOfParameters, isRefused] : { std::pair { 60000u, false }, std::pair { 70000u, true } }) {
        runOnBuildThread([&] {
            BuildContext build(makeOptionsOfTestedProgram());
            std::unique_ptr<Program> parsed = parseProgram(withStandInLibrary(functionWithManyParametersForGrowthTests(numberOfParameters)), build);
            AOT_TEST_CHECK(parsed);
            StagedBuild stages(WTF::move(parsed), build);
            AOT_TEST_CHECK(stages.facts());
            AOT_TEST_CHECK(!stages.plan() == isRefused);
            build.diagnostics().finish(nullptr);
            bool namesLimit = std::ranges::any_of(build.diagnostics().userDiagnostics(), [](const UserDiagnostic& diagnostic) {
                return diagnostic.kind == UserDiagnosticKind::LimitExceeded && diagnostic.limit == LimitID::MaxReservedStackArguments && diagnostic.function == FunctionID(1u);
            });
            AOT_TEST_CHECK(namesLimit == isRefused);
        });
    }
}

static String classesBelowOneClassForGrowthTests(unsigned numberOfClasses)
{
    StringBuilder text;
    text.append("layout L0 kind=Instance name=\"Base\" module=m0 constructor=f1\n"_s);
    for (unsigned number = 0; number < numberOfClasses; ++number)
        text.append("layout L"_s, number + 1, " kind=Instance name=\"C"_s, number, "\" module=m0 base=L0 constructor=f"_s, number + 2, " supertypes=[L0] flags=[HasHeritage]\n"_s);
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\nentry module=m0\n"_s);
    text.append("function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = Builtin builtin=GlobalThis\n    v1 = PropGet v0 key=\"sink\" [isFreeName]\n"_s);
    text.append("    v2 = NewClosure function=f1\n    v3 = NewClass v2 layout=L0\n    v4 = FinishClass v3 layout=L0\n    v5 = Convert v4 declared={type=f1 flags=[IsFromCompiler]}\n"_s);
    unsigned value = 6;
    for (unsigned number = 0; number < numberOfClasses; ++number, value += 7) {
        text.append("    v"_s, value, " = ResolveHeritage v5\n    v"_s, value + 1, " = NewClosure function=f"_s, number + 2, "\n    v"_s, value + 2, " = NewClass v"_s, value + 1, ", v5, v"_s, value, " layout=L"_s, number + 1, '\n');
        text.append("    v"_s, value + 3, " = FinishClass v"_s, value + 2, " layout=L"_s, number + 1, "\n    v"_s, value + 4, " = Convert v"_s, value + 3, " declared={type=f"_s, number + 2, " flags=[IsFromCompiler]}\n"_s);
        text.append("    v"_s, value + 5, " = Construct v"_s, value + 4, ", -\n    v"_s, value + 6, " = Call v1, -, v"_s, value + 5, '\n');
    }
    text.append("    v"_s, value, " = Undefined\n    Return v"_s, value, "\n}\n"_s);
    text.append("function f1 name=\"Base\" module=m0 kind=BaseConstructor flags=[HasNewTarget] thisType={type=anything} results=[{type=object flags=[IsFromCompiler]}] {\n  b0(v0: anything):\n"_s);
    text.append("    v1 = GetConstructPrototype v0 layout=L0\n    v2 = NewObject v1 layout=L0\n    v3 = Convert v2 declared={type=L0}\n    Return v3\n}\n"_s);
    for (unsigned number = 0; number < numberOfClasses; ++number) {
        text.append("function f"_s, number + 2, " name=\"C"_s, number, "\" module=m0 kind=DerivedConstructor flags=[HasNewTarget, TakesCallee] thisType={type=anything} results=[{type=object flags=[IsFromCompiler]}] {\n"_s);
        text.append("  b0(v0: function, v1: anything):\n    v2 = GetSuperBase v0\n    v3 = SuperConstruct v2, v1 layout=L"_s, number + 1, "\n    v4 = Convert v3 declared={type=L"_s, number + 1, "}\n    Return v4\n}\n"_s);
    }
    return text.toString();
}

static std::optional<uint64_t> instructionsOfFactsForGrowthTests(const String& text)
{
    std::optional<uint64_t> instructions;
    runOnBuildThread([&] {
        BuildContext build(makeOptionsOfTestedProgram());
        std::unique_ptr<Program> parsed = parseProgram(withStandInLibrary(text), build);
        AOT_TEST_CHECK(parsed);
        StagedBuild stages(WTF::move(parsed), build);
        instructions = instructionsOfAllThreadsIn([&] {
            AOT_TEST_CHECK(stages.facts());
        });
    });
    return instructions;
}

static void testFactsGrowAsClassesBelowOneClassDo()
{
    constexpr unsigned numberOfClasses = 3000;
    InstructionCounts::begin();
    std::optional<uint64_t> smaller = instructionsOfFactsForGrowthTests(classesBelowOneClassForGrowthTests(numberOfClasses));
    std::optional<uint64_t> large = instructionsOfFactsForGrowthTests(classesBelowOneClassForGrowthTests(4 * numberOfClasses));
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

static String objectsCreatedBelowOneLiteralForGrowthTests(unsigned numberOfSites)
{
    StringBuilder text;
    text.append("layout L0 module=m0 fields=[{key=\"code\" declared={type=anything} flags=[IsRequired]}] ownFieldCount=1\n"_s);
    for (unsigned site = 0; site < numberOfSites; ++site)
        text.append("layout L"_s, site + 1, " kind=Instance module=m0 flags=[IsOfCreatedObject]\n"_s);
    text.append("variable g0 name=\"above\" module=m0 declared={type=anything|absent} flags=[IsImmutable]\n"_s);
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\nentry module=m0\n"_s);
    text.append("function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = NewObject layout=L0\n    v1 = NewClosure function=f1\n    DefineGetter v0, v1 key=\"code\"\n"_s);
    text.append("    v3 = NewClosure function=f2\n    DefineSetter v0, v3 key=\"code\"\n    v5 = FinishObject v0\n    PutModuleVariable v5 variable=g0\n    v7 = Integer integer=1\n"_s);
    unsigned value = 8;
    for (unsigned site = 0; site < numberOfSites; ++site, value += 2)
        text.append("    v"_s, value, " = NewClosure function=f"_s, site + 3, "\n    v"_s, value + 1, " = Call v"_s, value, ", -, v7\n"_s);
    text.append("    v"_s, value, " = Undefined\n    Return v"_s, value, "\n}\n"_s);
    text.append("function f1 name=\"code\" module=m0 kind=Getter flags=[HasThis] thisType={type=anything} results=[{type=anything}] {\n  b0(v0: anything):\n    v1 = PropGet v0 key=\"held\"\n    Return v1\n}\n"_s);
    text.append("function f2 name=\"code\" module=m0 kind=Setter flags=[HasThis] length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=anything}] {\n  b0(v0: anything, v1: anything):\n"_s);
    text.append("    PropSet v0, v1 key=\"held\"\n    v3 = Undefined\n    Return v3\n}\n"_s);
    for (unsigned site = 0; site < numberOfSites; ++site) {
        text.append("function f"_s, site + 3, " name=\"make\" module=m0 length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=anything}] {\n  b0(v0: anything):\n"_s);
        text.append("    v1 = GetModuleVariable variable=g0\n    v2 = CheckInitialized v1 name=\"above\"\n    v3 = NewObject layout=L"_s, site + 1, "\n    SetLiteralPrototype v3, v2 [refusesOtherValues]\n"_s);
        text.append("    v5 = FinishObject v3\n    PropSet v5, v0 key=\"code\"\n    v7 = PropGet v5 key=\"code\"\n    Return v7\n}\n"_s);
    }
    return text.toString();
}

static void testFactsGrowAsObjectsCreatedBelowOneLiteralDo()
{
    constexpr unsigned numberOfSites = 1000;
    InstructionCounts::begin();
    std::optional<uint64_t> smaller = instructionsOfFactsForGrowthTests(objectsCreatedBelowOneLiteralForGrowthTests(numberOfSites));
    std::optional<uint64_t> large = instructionsOfFactsForGrowthTests(objectsCreatedBelowOneLiteralForGrowthTests(4 * numberOfSites));
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

enum class LoopInRowForGrowthTests : uint8_t {
    Empty,
    OverList,
};

static String loopsInRowForGrowthTests(LoopInRowForGrowthTests kind, unsigned numberOfLoops)
{
    StringBuilder text;
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\nentry module=m0\n"_s);
    text.append("function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = Builtin builtin=GlobalThis\n    v1 = PropGet v0 key=\"sink\" [isFreeName]\n"_s);
    text.append("    v2 = NewClosure function=f1\n    v3 = Call v1, -, v2\n    Return v3\n}\n"_s);
    text.append("function f1 name=\"run\" module=m0 length=1 thisType={type=anything} parameters=[{type=boolean}] results=[{type=anything}] {\n  b0(v0: boolean):\n    v1 = NewArray v0 contents=anything\n"_s);
    unsigned value = 2;
    unsigned block = 1;
    for (unsigned loop = 0; loop < numberOfLoops; ++loop) {
        switch (kind) {
        case LoopInRowForGrowthTests::Empty:
            text.append("    Jump -> next b"_s, block, "()\n  b"_s, block, "():\n    LoopHint\n    Branch v0 -> taken b"_s, block, "(), notTaken b"_s, block + 1, "()\n  b"_s, block + 1, "():\n"_s);
            value += 1;
            block += 2;
            break;
        case LoopInRowForGrowthTests::OverList:
            text.append("    v"_s, value, " = IteratorOpen v1\n    Jump -> next b"_s, block, "()\n  b"_s, block, "():\n    LoopHint\n"_s);
            text.append("    v"_s, value + 2, " = IteratorResume v"_s, value, "\n    v"_s, value + 3, " = ResultAt v"_s, value + 2, "\n    v"_s, value + 4, " = ResultAt v"_s, value + 2, " index=1\n"_s);
            text.append("    Branch v"_s, value + 3, " -> taken b"_s, block + 3, "(), notTaken b"_s, block + 1, "()\n"_s);
            text.append("  b"_s, block + 1, "() handler=b"_s, block + 2, "():\n    Jump -> next b"_s, block, "()\n"_s);
            text.append("  b"_s, block + 2, "() role=IterationCleanup subject=v"_s, value, ":\n    v"_s, value + 5, " = CatchException\n    IteratorClose v"_s, value, " completion=Throw\n    Rethrow v"_s, value + 5, "\n"_s);
            text.append("  b"_s, block + 3, "():\n"_s);
            value += 7;
            block += 4;
            break;
        }
    }
    text.append("    Return v0\n}\n"_s);
    return text.toString();
}

static std::optional<uint64_t> instructionsOfWalkForGrowthTests(const String& text, FunctionID function)
{
    std::optional<uint64_t> instructions;
    runOnBuildThread([&] {
        BuildContext build(makeOptionsOfTestedProgram());
        std::unique_ptr<Program> parsed = parseProgram(withStandInLibrary(text), build);
        AOT_TEST_CHECK(parsed);
        StagedBuild stages(WTF::move(parsed), build);
        AOT_TEST_CHECK(stages.code(CodeUnit { function, CodeRole::Body }));
        instructions = instructionsOfAllThreadsIn([&] {
            AOT_TEST_CHECK(stages.code(CodeUnit { function, CodeRole::Body }));
        });
    });
    return instructions;
}

static void testWalkGrowsAsLoopsInRowDo()
{
    constexpr unsigned numberOfLoops = 1500;
    InstructionCounts::begin();
    std::optional<uint64_t> smaller = instructionsOfWalkForGrowthTests(loopsInRowForGrowthTests(LoopInRowForGrowthTests::Empty, numberOfLoops), FunctionID(1u));
    std::optional<uint64_t> large = instructionsOfWalkForGrowthTests(loopsInRowForGrowthTests(LoopInRowForGrowthTests::Empty, 4 * numberOfLoops), FunctionID(1u));
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

static String birthsThatEachLeaveIntoOneMeetingForGrowthTests(unsigned numberOfBirths)
{
    StringBuilder text;
    text.append("layout L0 name=\"Point\" module=m0 fields=[{key=\"x\" declared={type=anything} flags=[IsRequired]}] ownFieldCount=1\nmodule m0 path=\"main.js\" displayPath=\"main.js\" body=f0\nentry module=m0\n"_s);
    text.append("function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = Builtin builtin=GlobalThis\n    v1 = PropGet v0 key=\"sink\" [isFreeName]\n    v2 = NewClosure function=f1\n    v3 = Call v1, -, v2\n    Return v3\n}\n"_s);
    text.append("function f1 name=\"run\" module=m0 length=3 thisType={type=anything} parameters=[{type=number}, {type=boolean}, {type=anything}] results=[{type=anything}] {\n  b0(v0: number, v1: boolean, v2: anything):\n    Switch v0 cases=["_s);
    for (unsigned birth = 0; birth < numberOfBirths; ++birth)
        text.append(birth ? ", "_s : ""_s, birth);
    text.append("] -> "_s);
    for (unsigned birth = 0; birth < numberOfBirths; ++birth)
        text.append("case b"_s, 2 * birth + 1, "(), "_s);
    unsigned meeting = 2 * numberOfBirths + 1;
    text.append("otherwise b"_s, meeting, "()\n"_s);
    for (unsigned birth = 0; birth < numberOfBirths; ++birth) {
        unsigned made = 3 * birth + 3;
        text.append("  b"_s, 2 * birth + 1, "():\n    v"_s, made, " = NewObject -, v2 layout=L0 fields=[0]\n    v"_s, made + 1, " = FinishObject v"_s, made, "\n    Branch v1 -> taken b"_s, 2 * birth + 2, "(), notTaken b"_s, meeting, "()\n"_s);
        text.append("  b"_s, 2 * birth + 2, "():\n    v"_s, made + 2, " = Call v2, -, v"_s, made + 1, "\n    Return v"_s, made + 2, "\n"_s);
    }
    text.append("  b"_s, meeting, "():\n    Return v2\n}\n"_s);
    return text.toString();
}

static std::optional<uint64_t> instructionsOfWalkThatMovesBirthsForGrowthTests(unsigned numberOfBirths)
{
    std::optional<uint64_t> instructions;
    runOnBuildThread([&] {
        BuildContext build(makeOptionsOfTestedProgram());
        std::unique_ptr<Program> parsed = parseProgram(withStandInLibrary(birthsThatEachLeaveIntoOneMeetingForGrowthTests(numberOfBirths)), build);
        AOT_TEST_CHECK(parsed);
        StagedBuild stages(WTF::move(parsed), build);
        AOT_TEST_CHECK(stages.code(CodeUnit { FunctionID(1u), CodeRole::Body }));
        AOT_TEST_CHECK(build.statistics().values()[static_cast<unsigned>(StatisticID::AllocationsSunk)] >= numberOfBirths);
        instructions = instructionsOfAllThreadsIn([&] {
            AOT_TEST_CHECK(stages.code(CodeUnit { FunctionID(1u), CodeRole::Body }));
        });
    });
    return instructions;
}

static void testSweepGrowsAsBirthsThatEachLeaveIntoOneMeetingDo()
{
    constexpr unsigned numberOfBirths = 4000;
    InstructionCounts::begin();
    std::optional<uint64_t> smaller = instructionsOfWalkThatMovesBirthsForGrowthTests(numberOfBirths);
    std::optional<uint64_t> large = instructionsOfWalkThatMovesBirthsForGrowthTests(4 * numberOfBirths);
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

static String loopsInRowThatHandSumOnInsideLoopForGrowthTests(unsigned numberOfLoops)
{
    StringBuilder text;
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\nentry module=m0\n"_s);
    text.append("function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = Builtin builtin=GlobalThis\n    v1 = PropGet v0 key=\"sink\" [isFreeName]\n"_s);
    text.append("    v2 = NewClosure function=f1\n    v3 = Call v1, -, v2\n    Return v3\n}\n"_s);
    text.append("function f1 name=\"run\" module=m0 length=1 thisType={type=anything} parameters=[{type=boolean}] results=[{type=anything}] {\n  b0(v0: boolean):\n    v1 = Integer\n    v2 = Convert v1 declared={type=number}\n    Jump -> next b1(v2)\n"_s);
    text.append("  b1(v3: number):\n    LoopHint\n    Branch v0 -> taken b2(v3), notTaken b"_s, 2 * numberOfLoops + 2, "(v3)\n"_s);
    for (unsigned loop = 0; loop < numberOfLoops; ++loop) {
        unsigned head = 2 * loop + 2;
        unsigned sum = 4 * loop + 5;
        text.append("  b"_s, head, "(v"_s, sum, ": number):\n    LoopHint\n    Branch v0 -> taken b"_s, head + 1, "(), notTaken b"_s, loop + 1 < numberOfLoops ? head + 2 : 1, "(v"_s, sum, ")\n"_s);
        text.append("  b"_s, head + 1, "():\n    v"_s, sum + 2, " = Increment v"_s, sum, "\n    v"_s, sum + 3, " = Convert v"_s, sum + 2, " declared={type=number}\n    Jump -> next b"_s, head, "(v"_s, sum + 3, ")\n"_s);
    }
    text.append("  b"_s, 2 * numberOfLoops + 2, "(v"_s, 4 * numberOfLoops + 5, ": number):\n    Return v"_s, 4 * numberOfLoops + 5, "\n}\n"_s);
    return text.toString();
}

static void testWalkGrowsAsLoopsInRowThatHandSumOnInsideLoopDo()
{
    constexpr unsigned numberOfLoops = 1500;
    InstructionCounts::begin();
    std::optional<uint64_t> smaller = instructionsOfWalkForGrowthTests(loopsInRowThatHandSumOnInsideLoopForGrowthTests(numberOfLoops), FunctionID(1u));
    std::optional<uint64_t> large = instructionsOfWalkForGrowthTests(loopsInRowThatHandSumOnInsideLoopForGrowthTests(4 * numberOfLoops), FunctionID(1u));
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

enum class ReckoningInLoopForGrowthTests : uint8_t {
    OneLongSumOfProducts,
    ManyShortSums,
    OneLongSumIntoNoNumber,
};

static String unknownValuesReckonedWithInLoopForGrowthTests(ReckoningInLoopForGrowthTests kind, unsigned numberOfValues)
{
    StringBuilder text;
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\nentry module=m0\n"_s);
    text.append("function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = Builtin builtin=GlobalThis\n    v1 = PropGet v0 key=\"sink\" [isFreeName]\n"_s);
    text.append("    v2 = NewClosure function=f1\n    v3 = Call v1, -, v2\n    Return v3\n}\n"_s);
    text.append("function f1 name=\"run\" module=m0 length=2 thisType={type=anything} parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {\n  b0(v0: anything, v1: anything):\n"_s);
    constexpr unsigned firstValue = 2;
    for (unsigned value = 0; value < numberOfValues; ++value)
        text.append("    v"_s, firstValue + value, " = PropGet v0 key=\"p"_s, value, "\"\n"_s);
    unsigned next = firstValue + numberOfValues;
    text.append("    v"_s, next, " = Integer\n    v"_s, next + 1, " = Convert v"_s, next, " declared={type=number flags=[IsExpected]}\n    v"_s, next + 2, " = Undefined\n"_s);
    text.append("    v"_s, next + 3, " = Integer\n    v"_s, next + 4, " = Convert v"_s, next + 3, " declared={type=number flags=[IsExpected]}\n    Jump -> next b1(v"_s, next + 1, ", v"_s, next + 2, ", v"_s, next + 4, ")\n"_s);
    unsigned sum = next + 5;
    unsigned other = next + 6;
    unsigned counter = next + 7;
    text.append("  b1(v"_s, sum, ", v"_s, other, ", v"_s, counter, "):\n    LoopHint\n    v"_s, next + 9, " = Less v"_s, counter, ", v1\n    Branch v"_s, next + 9, " -> taken b2(), notTaken b3()\n  b2():\n"_s);
    next += 10;
    unsigned sumOfTurn = sum;
    unsigned otherOfTurn = other;
    auto addToSum = [&](unsigned addend) {
        text.append("    v"_s, next, " = Add v"_s, sumOfTurn, ", v"_s, addend, "\n    v"_s, next + 1, " = Convert v"_s, next, " declared={type=number flags=[IsExpected]}\n"_s);
        sumOfTurn = next + 1;
        next += 2;
    };
    unsigned chain = 0;
    for (unsigned value = 0; value < numberOfValues; ++value) {
        unsigned term = firstValue + value;
        if (kind != ReckoningInLoopForGrowthTests::OneLongSumIntoNoNumber) {
            text.append("    v"_s, next, " = Mul v"_s, term, ", v"_s, term, "\n"_s);
            term = next++;
        }
        if (kind == ReckoningInLoopForGrowthTests::ManyShortSums) {
            addToSum(term);
            continue;
        }
        if (value) {
            text.append("    v"_s, next, " = Add v"_s, chain, ", v"_s, term, "\n"_s);
            term = next++;
        }
        chain = term;
    }
    if (kind == ReckoningInLoopForGrowthTests::OneLongSumOfProducts)
        addToSum(chain);
    if (kind == ReckoningInLoopForGrowthTests::OneLongSumIntoNoNumber)
        otherOfTurn = chain;
    text.append("    v"_s, next, " = Increment v"_s, counter, "\n    v"_s, next + 1, " = Convert v"_s, next, " declared={type=number flags=[IsExpected]}\n    Jump -> next b1(v"_s, sumOfTurn, ", v"_s, otherOfTurn, ", v"_s, next + 1, ")\n"_s);
    text.append("  b3():\n    v"_s, next + 2, " = NewArray v"_s, sum, ", v"_s, other, " contents=anything\n    Return v"_s, next + 2, "\n}\n"_s);
    return text.toString();
}

static void testWalkGrowsAsUnknownValuesReckonedWithInLoopDo()
{
    InstructionCounts::begin();
    for (auto [kind, numberOfValues] : { std::pair { ReckoningInLoopForGrowthTests::OneLongSumOfProducts, 1000u }, std::pair { ReckoningInLoopForGrowthTests::ManyShortSums, 4000u }, std::pair { ReckoningInLoopForGrowthTests::OneLongSumIntoNoNumber, 1000u } }) {
        std::optional<uint64_t> smaller = instructionsOfWalkForGrowthTests(unknownValuesReckonedWithInLoopForGrowthTests(kind, numberOfValues), FunctionID(1u));
        std::optional<uint64_t> large = instructionsOfWalkForGrowthTests(unknownValuesReckonedWithInLoopForGrowthTests(kind, 4 * numberOfValues), FunctionID(1u));
        AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
    }
}

enum class NamesOfLoopForGrowthTests : uint8_t {
    OfOneKind,
    EachOfItsOwnKind,
};

static String namesOfLoopThatEachTakeWhatManyOthersMeetInForGrowthTests(NamesOfLoopForGrowthTests kinds, unsigned numberOfNames)
{
    bool eachOfItsOwnKind = kinds == NamesOfLoopForGrowthTests::EachOfItsOwnKind;
    StringBuilder text;
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\nentry module=m0\n"_s);
    text.append("function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = Builtin builtin=GlobalThis\n    v1 = PropGet v0 key=\"sink\" [isFreeName]\n"_s);
    text.append("    v2 = NewClosure function=f1\n    v3 = Call v1, -, v2\n    Return v3\n}\n"_s);
    text.append("function f1 name=\"run\" module=m0 length=1 thisType={type=anything} parameters=[{type=boolean}] results=[{type=anything}] {\n  b0(v0: boolean):\n    v1 = Integer\n    v2 = Convert v1 declared={type=number}\n"_s);
    unsigned firstMade = 3;
    unsigned met = firstMade + (eachOfItsOwnKind ? numberOfNames : 0);
    unsigned firstGiver = met + 2;
    unsigned firstMeeting = firstGiver + 2 * numberOfNames + 1;
    for (unsigned name = 0; eachOfItsOwnKind && name < numberOfNames; ++name)
        text.append("    v"_s, firstMade + name, " = NewClosure function=f"_s, name + 2, "\n"_s);
    text.append("    Jump -> next b1(v2, v2"_s);
    for (unsigned name = 0; name < numberOfNames; ++name)
        text.append(", v"_s, eachOfItsOwnKind ? firstMade + name : 2);
    for (unsigned name = 0; name < numberOfNames; ++name)
        text.append(", v2"_s);
    text.append(")\n  b1(v"_s, met, ": anything, v"_s, met + 1, ": anything"_s);
    for (unsigned name = 0; name < 2 * numberOfNames; ++name)
        text.append(", v"_s, firstGiver + name, ": anything"_s);
    text.append("):\n    LoopHint\n    Branch v0 -> taken b2(), notTaken b"_s, 2 * numberOfNames + 3, "()\n  b2():\n"_s);
    for (unsigned name = 0; name < numberOfNames; ++name) {
        text.append("    Branch v0 -> taken b"_s, 2 * name + 3, "(), notTaken b"_s, 2 * name + 4, "(v"_s, name ? firstMeeting + name - 1 : met, ")\n"_s);
        text.append("  b"_s, 2 * name + 3, "():\n    Jump -> next b"_s, 2 * name + 4, "(v"_s, firstGiver + name, ")\n  b"_s, 2 * name + 4, "(v"_s, firstMeeting + name, ": anything):\n"_s);
    }
    unsigned last = firstMeeting + numberOfNames - 1;
    text.append("    Jump -> next b1(v"_s, last, ", v0"_s);
    for (unsigned name = 0; name < numberOfNames; ++name)
        text.append(", v"_s, firstGiver + name);
    for (unsigned name = 0; name < numberOfNames; ++name)
        text.append(", v"_s, last);
    text.append(")\n  b"_s, 2 * numberOfNames + 3, "():\n    Return v"_s, met, "\n}\n"_s);
    for (unsigned name = 0; eachOfItsOwnKind && name < numberOfNames; ++name)
        text.append("function f"_s, name + 2, " module=m0 thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = Integer\n    Return v0\n}\n"_s);
    return text.toString();
}

static void testWalkGrowsAsNamesOfLoopThatEachTakeWhatManyOthersMeetInDo()
{
    constexpr unsigned numberOfNames = 500;
    InstructionCounts::begin();
    for (NamesOfLoopForGrowthTests kinds : { NamesOfLoopForGrowthTests::OfOneKind, NamesOfLoopForGrowthTests::EachOfItsOwnKind }) {
        std::optional<uint64_t> smaller = instructionsOfWalkForGrowthTests(namesOfLoopThatEachTakeWhatManyOthersMeetInForGrowthTests(kinds, numberOfNames), FunctionID(1u));
        std::optional<uint64_t> large = instructionsOfWalkForGrowthTests(namesOfLoopThatEachTakeWhatManyOthersMeetInForGrowthTests(kinds, 4 * numberOfNames), FunctionID(1u));
        AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
    }
}

static String namesOfLoopThatMayEachTakeFromBothNeighborsForGrowthTests(unsigned numberOfNames)
{
    StringBuilder text;
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\nentry module=m0\n"_s);
    text.append("function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = Builtin builtin=GlobalThis\n    v1 = PropGet v0 key=\"sink\" [isFreeName]\n"_s);
    text.append("    v2 = NewClosure function=f1\n    v3 = Call v1, -, v2\n    Return v3\n}\n"_s);
    text.append("function f1 name=\"run\" module=m0 length=1 thisType={type=anything} parameters=[{type=boolean}] results=[{type=anything}] {\n  b0(v0: boolean):\n    v1 = Integer\n    v2 = Convert v1 declared={type=number}\n"_s);
    unsigned firstMade = 3;
    unsigned firstName = firstMade + numberOfNames + 1;
    unsigned firstTakenFromAbove = firstName + numberOfNames + 1;
    unsigned firstTakenFromBelow = firstTakenFromAbove + numberOfNames - 1;
    for (unsigned name = 0; name < numberOfNames; ++name)
        text.append("    v"_s, firstMade + name, " = NewClosure function=f"_s, name + 2, "\n"_s);
    text.append("    Jump -> next b1(v2"_s);
    for (unsigned name = 0; name < numberOfNames; ++name)
        text.append(", v"_s, firstMade + name);
    text.append(")\n  b1(v"_s, firstName - 1, ": anything"_s);
    for (unsigned name = 0; name < numberOfNames; ++name)
        text.append(", v"_s, firstName + name, ": anything"_s);
    text.append("):\n    LoopHint\n    Branch v0 -> taken b2(), notTaken b"_s, 4 * numberOfNames - 1, "()\n  b2():\n"_s);
    unsigned block = 3;
    auto mayTake = [&](unsigned held, unsigned taken, unsigned met) {
        text.append("    Branch v0 -> taken b"_s, block, "(), notTaken b"_s, block + 1, "(v"_s, held, ")\n  b"_s, block, "():\n    Jump -> next b"_s, block + 1, "(v"_s, taken, ")\n  b"_s, block + 1, "(v"_s, met, ": anything):\n"_s);
        block += 2;
    };
    for (unsigned name = 0; name + 1 < numberOfNames; ++name)
        mayTake(firstName + name, firstName + name + 1, firstTakenFromAbove + name);
    for (unsigned name = numberOfNames - 1; name; --name)
        mayTake(name + 1 < numberOfNames ? firstTakenFromAbove + name : firstName + name, firstTakenFromAbove + name - 1, firstTakenFromBelow + numberOfNames - 1 - name);
    text.append("    Jump -> next b1(v0, v"_s, firstTakenFromAbove);
    for (unsigned name = 1; name < numberOfNames; ++name)
        text.append(", v"_s, firstTakenFromBelow + numberOfNames - 1 - name);
    text.append(")\n  b"_s, 4 * numberOfNames - 1, "():\n    Return v"_s, firstName - 1, "\n}\n"_s);
    for (unsigned name = 0; name < numberOfNames; ++name)
        text.append("function f"_s, name + 2, " module=m0 thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = Integer\n    Return v0\n}\n"_s);
    return text.toString();
}

static void testWalkGrowsAsNamesOfLoopThatMayEachTakeFromBothNeighborsDo()
{
    constexpr unsigned numberOfNames = 500;
    InstructionCounts::begin();
    std::optional<uint64_t> smaller = instructionsOfWalkForGrowthTests(namesOfLoopThatMayEachTakeFromBothNeighborsForGrowthTests(numberOfNames), FunctionID(1u));
    std::optional<uint64_t> large = instructionsOfWalkForGrowthTests(namesOfLoopThatMayEachTakeFromBothNeighborsForGrowthTests(4 * numberOfNames), FunctionID(1u));
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

static String loopsInsideOneAnotherForGrowthTests(unsigned numberOfLoops)
{
    StringBuilder text;
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\nentry module=m0\n"_s);
    text.append("function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = Builtin builtin=GlobalThis\n    v1 = PropGet v0 key=\"sink\" [isFreeName]\n"_s);
    text.append("    v2 = NewClosure function=f1\n    v3 = Call v1, -, v2\n    Return v3\n}\n"_s);
    text.append("function f1 name=\"run\" module=m0 length=1 thisType={type=anything} parameters=[{type=boolean}] results=[{type=anything}] {\n  b0(v0: boolean):\n    Jump -> next b1()\n"_s);
    for (unsigned loop = 1; loop <= numberOfLoops; ++loop)
        text.append("  b"_s, loop, "():\n    LoopHint\n    Branch v0 -> taken b"_s, std::min(loop + 1, numberOfLoops), "(), notTaken b"_s, loop > 1 ? loop - 1 : numberOfLoops + 1, "()\n"_s);
    text.append("  b"_s, numberOfLoops + 1, "():\n    Return v0\n}\n"_s);
    return text.toString();
}

static String loopsInsideOneAnotherRoundSumForGrowthTests(unsigned numberOfLoops)
{
    StringBuilder text;
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\nentry module=m0\n"_s);
    text.append("function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = Builtin builtin=GlobalThis\n    v1 = PropGet v0 key=\"sink\" [isFreeName]\n"_s);
    text.append("    v2 = NewClosure function=f1\n    v3 = Call v1, -, v2\n    Return v3\n}\n"_s);
    text.append("function f1 name=\"run\" module=m0 length=1 thisType={type=anything} parameters=[{type=boolean}] results=[{type=anything}] {\n  b0(v0: boolean):\n    v1 = Integer\n    v2 = Convert v1 declared={type=number}\n    Jump -> next b1(v2)\n"_s);
    for (unsigned loop = 1; loop < numberOfLoops; ++loop)
        text.append("  b"_s, loop, "(v"_s, 2 * loop + 1, ": number):\n    LoopHint\n    Branch v0 -> taken b"_s, loop + 1, "(v"_s, 2 * loop + 1, "), notTaken b"_s, loop > 1 ? loop - 1 : numberOfLoops + 1, "(v"_s, 2 * loop + 1, ")\n"_s);
    unsigned sum = 2 * numberOfLoops + 1;
    text.append("  b"_s, numberOfLoops, "(v"_s, sum, ": number):\n    LoopHint\n    v"_s, sum + 2, " = Increment v"_s, sum, "\n    v"_s, sum + 3, " = Convert v"_s, sum + 2, " declared={type=number}\n"_s);
    text.append("    Branch v0 -> taken b"_s, numberOfLoops, "(v"_s, sum + 3, "), notTaken b"_s, numberOfLoops - 1, "(v"_s, sum + 3, ")\n  b"_s, numberOfLoops + 1, "(v"_s, sum + 4, ": number):\n    Return v"_s, sum + 4, "\n}\n"_s);
    return text.toString();
}

static NEVER_INLINE void runBelowMegabytesOfStackForGrowthTests(unsigned megabytes, NOESCAPE const ScopedLambda<void()>& functor)
{
    volatile char taken[MB];
    taken[0] = static_cast<char>(megabytes);
    if (megabytes)
        runBelowMegabytesOfStackForGrowthTests(megabytes - 1, functor);
    else
        functor();
    taken[MB - 1] = taken[0];
}

static uint64_t walksOfLoopsForGrowthTests(const String& text, unsigned megabytesOfStackTakenFirst)
{
    uint64_t walks = 0;
    runOnBuildThread([&] {
        BuildContext build(makeOptionsOfTestedProgram());
        std::unique_ptr<Program> parsed = parseProgram(withStandInLibrary(text), build);
        AOT_TEST_CHECK(parsed);
        StagedBuild stages(WTF::move(parsed), build);
        AOT_TEST_CHECK(stages.plan());
        runBelowMegabytesOfStackForGrowthTests(megabytesOfStackTakenFirst, [&] {
            AOT_TEST_CHECK(stages.code(CodeUnit { FunctionID(1u), CodeRole::Body }));
        });
        walks = build.statistics().values()[static_cast<unsigned>(StatisticID::LoopVisits)];
    });
    return walks;
}

static void testNestOfLoopsIsWalkedAlikeWhateverRoomStackHas()
{
    uint64_t walks = walksOfLoopsForGrowthTests(loopsInsideOneAnotherForGrowthTests(1200), 0);
    AOT_TEST_CHECK(walks == walksOfLoopsForGrowthTests(loopsInsideOneAnotherForGrowthTests(1200), 4));
    AOT_TEST_CHECK(walks == walksOfLoopsForGrowthTests(loopsInsideOneAnotherForGrowthTests(1300), 0));
}

static void testNestDeeperThanTriesGoIsWalkedLittleMoreThanOneThatIsNot()
{
    AOT_TEST_CHECK(walksOfLoopsForGrowthTests(loopsInsideOneAnotherRoundSumForGrowthTests(1000), 0) <= 4 * 1000);
    AOT_TEST_CHECK(walksOfLoopsForGrowthTests(loopsInsideOneAnotherRoundSumForGrowthTests(1200), 0) <= 4 * 1200);
}

static String loopsInsideOneAnotherOverHoldersOfTheirOwnForGrowthTests(unsigned numberOfLoops)
{
    constexpr ASCIILiteral asCounter = " declared={type=i32 flags=[IsExpected]}\n"_s;
    constexpr ASCIILiteral asSum = " declared={type=number flags=[IsExpected]}\n"_s;
    StringBuilder text;
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\nentry module=m0\n"_s);
    text.append("function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = Builtin builtin=GlobalThis\n    v1 = PropGet v0 key=\"sink\" [isFreeName]\n"_s);
    text.append("    v2 = NewClosure function=f1\n    v3 = Call v1, -, v2\n    Return v3\n}\n"_s);
    text.append("function f1 name=\"run\" module=m0 length="_s, 3 * numberOfLoops, " thisType={type=anything} parameters=["_s);
    for (unsigned holder = 0; holder < 3 * numberOfLoops; ++holder)
        text.append(holder ? ", "_s : ""_s, "{type=undefined|null|Array<number> flags=[IsExpected]}"_s);
    text.append("] results=[{type=anything}] {\n  b0("_s);
    for (unsigned holder = 0; holder < 3 * numberOfLoops; ++holder)
        text.append(holder ? ", v"_s : "v"_s, holder, ": undefined|null|Array<number>"_s);
    unsigned value = 3 * numberOfLoops;
    text.append("):\n    v"_s, value, " = Integer\n    v"_s, value + 1, " = Convert v"_s, value, asSum, "    v"_s, value + 2, " = Integer\n    v"_s, value + 3, " = Convert v"_s, value + 2, asCounter);
    text.append("    Jump -> next b1(v"_s, value + 1, ", v"_s, value + 3, ")\n"_s);
    value += 4;
    Vector<unsigned> sums;
    Vector<unsigned> counters;
    auto blockBehind = [&](unsigned loop) {
        return loop ? 3 * numberOfLoops - loop : 3 * numberOfLoops;
    };
    for (unsigned loop = 0; loop < numberOfLoops; ++loop) {
        unsigned sum = value;
        unsigned counter = value + 1;
        sums.append(sum);
        counters.append(counter);
        text.append("  b"_s, 2 * loop + 1, "(v"_s, sum, ", v"_s, counter, "):\n    LoopHint\n    v"_s, value + 3, " = PropGet v"_s, 3 * loop, " key=\"length\"\n    v"_s, value + 4, " = Less v"_s, counter, ", v"_s, value + 3, '\n');
        text.append("    Branch v"_s, value + 4, " -> taken b"_s, 2 * loop + 2, "(), notTaken b"_s, blockBehind(loop), "()\n  b"_s, 2 * loop + 2, "():\n"_s);
        text.append("    v"_s, value + 5, " = ElemGet v"_s, 3 * loop, ", v"_s, counter, "\n    v"_s, value + 6, " = Integer integer=2\n    v"_s, value + 7, " = Mul v"_s, value + 5, ", v"_s, value + 6, '\n');
        text.append("    v"_s, value + 8, " = ElemGet v"_s, 3 * loop + 1, ", v"_s, counter, "\n    v"_s, value + 9, " = Add v"_s, value + 7, ", v"_s, value + 8, '\n');
        text.append("    ElemSet v"_s, 3 * loop + 2, ", v"_s, counter, ", v"_s, value + 9, " mode=Sloppy\n"_s);
        value += 11;
        if (loop + 1 < numberOfLoops) {
            text.append("    v"_s, value, " = Integer\n    v"_s, value + 1, " = Convert v"_s, value, asCounter, "    Jump -> next b"_s, 2 * loop + 3, "(v"_s, sum, ", v"_s, value + 1, ")\n"_s);
            value += 2;
            continue;
        }
        text.append("    v"_s, value, " = ElemGet v"_s, 3 * loop + 2, ", v"_s, counter, "\n    v"_s, value + 1, " = Add v"_s, sum, ", v"_s, value, "\n    v"_s, value + 2, " = Convert v"_s, value + 1, asSum);
        text.append("    v"_s, value + 3, " = Increment v"_s, counter, "\n    v"_s, value + 4, " = Convert v"_s, value + 3, asCounter, "    Jump -> next b"_s, 2 * loop + 1, "(v"_s, value + 2, ", v"_s, value + 4, ")\n"_s);
        value += 5;
    }
    for (unsigned loop = numberOfLoops - 1; loop--;) {
        text.append("  b"_s, blockBehind(loop + 1), "():\n    v"_s, value, " = Increment v"_s, counters[loop], "\n    v"_s, value + 1, " = Convert v"_s, value, asCounter);
        text.append("    Jump -> next b"_s, 2 * loop + 1, "(v"_s, sums[loop + 1], ", v"_s, value + 1, ")\n"_s);
        value += 2;
    }
    text.append("  b"_s, 3 * numberOfLoops, "():\n    Return v"_s, sums[0], "\n}\n"_s);
    return text.toString();
}

static void testNestOfLoopsOverHoldersOfTheirOwnIsWalkedNoMoreThanItWas()
{
    AOT_TEST_CHECK(walksOfLoopsForGrowthTests(loopsInsideOneAnotherOverHoldersOfTheirOwnForGrowthTests(2), 0) <= 770);
    AOT_TEST_CHECK(walksOfLoopsForGrowthTests(loopsInsideOneAnotherOverHoldersOfTheirOwnForGrowthTests(4), 0) <= 5500);
    AOT_TEST_CHECK(walksOfLoopsForGrowthTests(loopsInsideOneAnotherOverHoldersOfTheirOwnForGrowthTests(8), 0) <= 41000);
}

static String loopThatReadsManyListsUnderOneCounterForGrowthTests(unsigned numberOfLists)
{
    constexpr ASCIILiteral asCounter = " declared={type=i32 flags=[IsExpected]}\n"_s;
    constexpr ASCIILiteral asSum = " declared={type=number flags=[IsExpected]}\n"_s;
    StringBuilder text;
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\nentry module=m0\n"_s);
    text.append("function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = Builtin builtin=GlobalThis\n    v1 = PropGet v0 key=\"sink\" [isFreeName]\n"_s);
    text.append("    v2 = NewClosure function=f1\n    v3 = Call v1, -, v2\n    Return v3\n}\n"_s);
    text.append("function f1 name=\"run\" module=m0 length="_s, numberOfLists, " thisType={type=anything} parameters=["_s);
    for (unsigned list = 0; list < numberOfLists; ++list)
        text.append(list ? ", "_s : ""_s, "{type=undefined|null|Array<number> flags=[IsExpected]}"_s);
    text.append("] results=[{type=anything}] {\n  b0("_s);
    for (unsigned list = 0; list < numberOfLists; ++list)
        text.append(list ? ", v"_s : "v"_s, list, ": undefined|null|Array<number>"_s);
    unsigned value = numberOfLists;
    text.append("):\n    v"_s, value, " = Integer\n    v"_s, value + 1, " = Convert v"_s, value, asSum, "    v"_s, value + 2, " = Integer\n    v"_s, value + 3, " = Convert v"_s, value + 2, asCounter);
    text.append("    Jump -> next b1(v"_s, value + 1, ", v"_s, value + 3, ")\n"_s);
    value += 4;
    unsigned sum = value;
    unsigned counter = value + 1;
    text.append("  b1(v"_s, sum, ", v"_s, counter, "):\n    LoopHint\n    v"_s, value + 3, " = PropGet v0 key=\"length\"\n    v"_s, value + 4, " = Less v"_s, counter, ", v"_s, value + 3, '\n');
    text.append("    Branch v"_s, value + 4, " -> taken b2(), notTaken b3()\n  b2():\n"_s);
    value += 5;
    unsigned running = sum;
    for (unsigned list = 0; list < numberOfLists; ++list, value += 5) {
        text.append("    v"_s, value, " = ElemGet v"_s, list, ", v"_s, counter, "\n    v"_s, value + 1, " = Integer integer=2\n    v"_s, value + 2, " = Mul v"_s, value, ", v"_s, value + 1, '\n');
        text.append("    v"_s, value + 3, " = Add v"_s, running, ", v"_s, value + 2, "\n    v"_s, value + 4, " = Convert v"_s, value + 3, asSum);
        running = value + 4;
    }
    text.append("    v"_s, value, " = Increment v"_s, counter, "\n    v"_s, value + 1, " = Convert v"_s, value, asCounter, "    Jump -> next b1(v"_s, running, ", v"_s, value + 1, ")\n"_s);
    text.append("  b3():\n    Return v"_s, sum, "\n}\n"_s);
    return text.toString();
}

static void testWalkOfLoopThatReadsManyListsUnderOneCounterGrowsNoFasterThanItDid()
{
    InstructionCounts::begin();
    std::optional<uint64_t> smaller = instructionsOfWalkForGrowthTests(loopThatReadsManyListsUnderOneCounterForGrowthTests(100), FunctionID(1u));
    std::optional<uint64_t> large = instructionsOfWalkForGrowthTests(loopThatReadsManyListsUnderOneCounterForGrowthTests(400), FunctionID(1u));
    AOT_TEST_CHECK(!smaller || !large || 10 * *large < 48 * *smaller);
}

static void testScratchRealmHasItsRoomWhereverQuestionStands()
{
    runOnBuildThread([&] {
        ScratchRealm realm;
        auto hasRoom = [&] {
            return realm.run([](JSGlobalObject* global) {
                return global->vm().isSafeToRecurse();
            });
        };
        AOT_TEST_CHECK(hasRoom());
        runBelowMegabytesOfStackForGrowthTests(6, [&] {
            AOT_TEST_CHECK(hasRoom());
        });
    });
}

static String loopsUpToOneBoundForGrowthTests(unsigned numberOfPairs)
{
    StringBuilder text;
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\nentry module=m0\n"_s);
    text.append("function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = Builtin builtin=GlobalThis\n    v1 = PropGet v0 key=\"sink\" [isFreeName]\n"_s);
    text.append("    v2 = NewClosure function=f1\n    v3 = Call v1, -, v2\n    Return v3\n}\n"_s);
    text.append("function f1 name=\"run\" module=m0 length=1 thisType={type=anything} parameters=[{type=int[0, 4294967295]}] results=[{type=anything}] {\n  b0(v0: int[0, 4294967295]):\n"_s);
    text.append("    v1 = Integer\n    v2 = Convert v1 declared={type=number}\n"_s);
    constexpr ASCIILiteral asNumber = " declared={type=number}\n"_s;
    unsigned value = 3;
    unsigned block = 1;
    unsigned sum = 2;
    for (unsigned pair = 0; pair < numberOfPairs; ++pair, value += 22, block += 6) {
        text.append("    v"_s, value, " = Integer\n    v"_s, value + 1, " = Convert v"_s, value, asNumber, "    Jump -> next b"_s, block, "(v"_s, sum, ", v"_s, value + 1, ")\n"_s);
        text.append("  b"_s, block, "(v"_s, value + 2, ": number, v"_s, value + 3, ": number):\n    LoopHint\n    v"_s, value + 5, " = Less v"_s, value + 3, ", v0\n"_s);
        text.append("    Branch v"_s, value + 5, " -> taken b"_s, block + 1, "(), notTaken b"_s, block + 2, "()\n"_s);
        text.append("  b"_s, block + 1, "():\n    v"_s, value + 6, " = Add v"_s, value + 2, ", v"_s, value + 3, "\n    v"_s, value + 7, " = Convert v"_s, value + 6, asNumber);
        text.append("    v"_s, value + 8, " = Increment v"_s, value + 3, "\n    v"_s, value + 9, " = Convert v"_s, value + 8, asNumber, "    Jump -> next b"_s, block, "(v"_s, value + 7, ", v"_s, value + 9, ")\n"_s);
        text.append("  b"_s, block + 2, "():\n    v"_s, value + 10, " = Integer integer=2\n    v"_s, value + 11, " = Convert v"_s, value + 10, asNumber, "    Jump -> next b"_s, block + 3, "(v"_s, value + 2, ", v"_s, value + 11, ")\n"_s);
        text.append("  b"_s, block + 3, "(v"_s, value + 12, ": number, v"_s, value + 13, ": number):\n    LoopHint\n    v"_s, value + 15, " = ToNumeric v"_s, value + 13, "\n    v"_s, value + 16, " = Decrement v"_s, value + 15, '\n');
        text.append("    v"_s, value + 17, " = Convert v"_s, value + 16, asNumber, "    v"_s, value + 18, " = Integer\n    v"_s, value + 19, " = Greater v"_s, value + 15, ", v"_s, value + 18, '\n');
        text.append("    Branch v"_s, value + 19, " -> taken b"_s, block + 4, "(), notTaken b"_s, block + 5, "()\n"_s);
        text.append("  b"_s, block + 4, "():\n    v"_s, value + 20, " = Add v"_s, value + 12, ", v"_s, value + 17, "\n    v"_s, value + 21, " = Convert v"_s, value + 20, asNumber, "    Jump -> next b"_s, block + 3, "(v"_s, value + 21, ", v"_s, value + 17, ")\n"_s);
        text.append("  b"_s, block + 5, "():\n"_s);
        sum = value + 12;
    }
    text.append("    Return v"_s, sum, "\n}\n"_s);
    return text.toString();
}

static void testWalkGrowsAsLoopsUpToOneBoundDo()
{
    constexpr unsigned numberOfPairs = 3000;
    InstructionCounts::begin();
    std::optional<uint64_t> smaller = instructionsOfWalkForGrowthTests(loopsUpToOneBoundForGrowthTests(numberOfPairs), FunctionID(1u));
    std::optional<uint64_t> large = instructionsOfWalkForGrowthTests(loopsUpToOneBoundForGrowthTests(4 * numberOfPairs), FunctionID(1u));
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

static void testWalkGrowsAsIterationsInRowDo()
{
    constexpr unsigned numberOfLoops = 3000;
    InstructionCounts::begin();
    std::optional<uint64_t> smaller = instructionsOfWalkForGrowthTests(loopsInRowForGrowthTests(LoopInRowForGrowthTests::OverList, numberOfLoops), FunctionID(1u));
    std::optional<uint64_t> large = instructionsOfWalkForGrowthTests(loopsInRowForGrowthTests(LoopInRowForGrowthTests::OverList, 4 * numberOfLoops), FunctionID(1u));
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

enum class LoopsOverGeneratorsForGrowthTests : uint8_t {
    YieldsAndStatements,
    InsideOneAnother,
    InsideOneAnotherOverLists,
    OneBehindAnother,
    GeneratorsOverOneAnother,
};

static void appendHeadOfGeneratorForGrowthTests(StringBuilder& text, unsigned function)
{
    text.append("function f"_s, function, " name=\"made"_s, function, "\" module=m0 flags=[IsGenerator, HasThis, TakesCallee, IsChecked] length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=anything}] {\n"_s);
    text.append("  b0(v0: anything, v1: function, v2: anything):\n    InitialYield v2, v0\n"_s);
}

static void appendGeneratorOfYieldsInRowForGrowthTests(StringBuilder& text, unsigned function, unsigned numberOfYields)
{
    appendHeadOfGeneratorForGrowthTests(text, function);
    unsigned value = 4;
    unsigned block = 1;
    for (unsigned yield = 0; yield < numberOfYields; ++yield) {
        text.append("    v"_s, value, " = Yield v2, v2, v0\n    v"_s, value + 1, " = ResultAt v"_s, value, "\n    v"_s, value + 2, " = ResultAt v"_s, value, " index=1\n"_s);
        text.append("    Switch v"_s, value + 2, " cases=[1, 2] -> case b"_s, block, "(), case b"_s, block + 1, "(), otherwise b"_s, block + 2, "()\n"_s);
        text.append("  b"_s, block, "():\n    Throw v"_s, value + 1, "\n  b"_s, block + 1, "():\n    Return v"_s, value + 1, "\n  b"_s, block + 2, "():\n"_s);
        value += 3;
        block += 3;
    }
    text.append("    v"_s, value, " = Undefined\n    Return v"_s, value, "\n}\n"_s);
}

static void appendGeneratorOverGeneratorForGrowthTests(StringBuilder& text, unsigned function, unsigned variableOfInner, unsigned numberOfYields)
{
    appendHeadOfGeneratorForGrowthTests(text, function);
    text.append("    v4 = GetModuleVariable variable=g"_s, variableOfInner, "\n    v5 = Call v4, -, v2\n    v6 = IteratorOpen v5\n    Jump -> next b1()\n"_s);
    text.append("  b1():\n    LoopHint\n    v8 = IteratorResume v6\n    v9 = ResultAt v8\n    v10 = ResultAt v8 index=1\n    Branch v9 -> taken b2(), notTaken b3()\n"_s);
    text.append("  b2():\n    v11 = Undefined\n    Return v11\n  b3():\n    Jump -> next b4()\n"_s);
    unsigned close = 4 + 3 * numberOfYields;
    unsigned back = close + 1;
    unsigned cleanup = close + 2;
    unsigned value = 12;
    unsigned block = 4;
    for (unsigned yield = 0; yield < numberOfYields; ++yield) {
        text.append("  b"_s, block, "() handler=b"_s, cleanup, "():\n    v"_s, value, " = Yield v10, v2, v0\n    v"_s, value + 1, " = ResultAt v"_s, value, "\n    v"_s, value + 2, " = ResultAt v"_s, value, " index=1\n"_s);
        text.append("    Switch v"_s, value + 2, " cases=[1, 2] -> case b"_s, block + 1, "(), case b"_s, block + 2, "(), otherwise b"_s, yield + 1 < numberOfYields ? block + 3 : back, "()\n"_s);
        text.append("  b"_s, block + 1, "() handler=b"_s, cleanup, "():\n    Throw v"_s, value + 1, "\n  b"_s, block + 2, "() handler=b"_s, cleanup, "():\n    Jump -> next b"_s, close, "(v"_s, value + 1, ")\n"_s);
        value += 3;
        block += 3;
    }
    text.append("  b"_s, close, "(v"_s, value, "):\n    IteratorClose v6\n    Return v"_s, value, "\n  b"_s, back, "() handler=b"_s, cleanup, "():\n    Jump -> next b1()\n"_s);
    text.append("  b"_s, cleanup, "() role=IterationCleanup subject=v6:\n    v"_s, value + 2, " = CatchException\n    IteratorClose v6 completion=Throw\n    Rethrow v"_s, value + 2, "\n}\n"_s);
}

static void appendLoopOverOneGeneratorForGrowthTests(StringBuilder& text, unsigned variable, unsigned numberOfStatements)
{
    text.append("    v1 = GetModuleVariable variable=g"_s, variable, "\n    v2 = Call v1, -, v0\n    v3 = IteratorOpen v2\n    Jump -> next b1(v0)\n"_s);
    text.append("  b1(v4: number):\n    LoopHint\n    v6 = IteratorResume v3\n    v7 = ResultAt v6\n    v8 = ResultAt v6 index=1\n    Branch v7 -> taken b2(), notTaken b3()\n"_s);
    text.append("  b2():\n    Return v4\n  b3():\n    Jump -> next b4()\n  b4() handler=b5():\n"_s);
    unsigned value = 9;
    for (unsigned statement = 0; statement < numberOfStatements; ++statement, ++value)
        text.append("    v"_s, value, " = BitXor v"_s, statement ? value - 1 : 4, ", v0\n"_s);
    text.append("    Jump -> next b1(v"_s, value - 1, ")\n  b5() role=IterationCleanup subject=v3:\n    v"_s, value, " = CatchException\n    IteratorClose v3 completion=Throw\n    Rethrow v"_s, value, "\n}\n"_s);
}

static String loopsOverGeneratorsForGrowthTests(LoopsOverGeneratorsForGrowthTests kind, unsigned size)
{
    StringBuilder text;
    unsigned numberOfGenerators = kind == LoopsOverGeneratorsForGrowthTests::YieldsAndStatements || kind == LoopsOverGeneratorsForGrowthTests::OneBehindAnother ? 1 : size;
    for (unsigned generator = 0; generator < numberOfGenerators; ++generator)
        text.append("variable g"_s, generator, " name=\"made"_s, generator + 2, "\" module=m0 declared={type=f"_s, generator + 2, " flags=[IsFromCompiler]} initialization=Function function=f"_s, generator + 2, " flags=[IsImmutable]\n"_s);
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\" body=f0 namespaceLayout=L(m0)\nentry module=m0\n"_s);
    text.append("function f0 module=m0 kind=Module flags=[IsChecked] thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = NewClosure function=f1\n    v1 = Integer integer=1\n    v2 = Call v0, -, v1\n    Return v2\n}\n"_s);
    text.append("function f1 name=\"run\" module=m0 flags=[IsChecked] length=1 thisType={type=anything} parameters=[{type=number}] results=[{type=anything}] {\n  b0(v0: number):\n"_s);

    switch (kind) {
    case LoopsOverGeneratorsForGrowthTests::YieldsAndStatements:
        appendLoopOverOneGeneratorForGrowthTests(text, 0, size);
        appendGeneratorOfYieldsInRowForGrowthTests(text, 2, size);
        break;
    case LoopsOverGeneratorsForGrowthTests::GeneratorsOverOneAnother:
        appendLoopOverOneGeneratorForGrowthTests(text, size - 1, 1);
        appendGeneratorOfYieldsInRowForGrowthTests(text, 2, 2);
        for (unsigned generator = 1; generator < size; ++generator)
            appendGeneratorOverGeneratorForGrowthTests(text, generator + 2, generator - 1, 2);
        break;
    case LoopsOverGeneratorsForGrowthTests::OneBehindAnother: {
        unsigned head = 4;
        for (unsigned loop = 0; loop < size; ++loop, head += 11) {
            unsigned block = 5 * loop;
            text.append("    v"_s, head - 3, " = GetModuleVariable variable=g0\n    v"_s, head - 2, " = Call v"_s, head - 3, ", -, v0\n    v"_s, head - 1, " = IteratorOpen v"_s, head - 2, '\n');
            text.append("    Jump -> next b"_s, block + 1, "(v"_s, loop ? head - 11 : 0, ")\n  b"_s, block + 1, "(v"_s, head, ": number):\n    LoopHint\n    v"_s, head + 2, " = IteratorResume v"_s, head - 1, '\n');
            text.append("    v"_s, head + 3, " = ResultAt v"_s, head + 2, "\n    v"_s, head + 4, " = ResultAt v"_s, head + 2, " index=1\n    Branch v"_s, head + 3, " -> taken b"_s, block + 5, "(), notTaken b"_s, block + 2, "()\n"_s);
            text.append("  b"_s, block + 2, "():\n    Jump -> next b"_s, block + 3, "()\n  b"_s, block + 3, "() handler=b"_s, block + 4, "():\n    v"_s, head + 5, " = BitXor v"_s, head, ", v0\n    Jump -> next b"_s, block + 1, "(v"_s, head + 5, ")\n"_s);
            text.append("  b"_s, block + 4, "() role=IterationCleanup subject=v"_s, head - 1, ":\n    v"_s, head + 6, " = CatchException\n    IteratorClose v"_s, head - 1, " completion=Throw\n    Rethrow v"_s, head + 6, "\n  b"_s, block + 5, "():\n"_s);
        }
        text.append("    Return v"_s, head - 11, "\n}\n"_s);
        appendGeneratorOfYieldsInRowForGrowthTests(text, 2, 2);
        break;
    }
    case LoopsOverGeneratorsForGrowthTests::InsideOneAnotherOverLists:
    case LoopsOverGeneratorsForGrowthTests::InsideOneAnother: {
        auto cleanupOf = [&](unsigned level) {
            return 3 * size + 1 + (size - 1 - level);
        };
        auto appendHandlerAround = [&](unsigned level) {
            if (level)
                text.append(" handler=b"_s, cleanupOf(level - 1), "()"_s);
        };
        unsigned value = 1;
        for (unsigned level = 0; level < size; ++level, value += 8) {
            if (kind == LoopsOverGeneratorsForGrowthTests::InsideOneAnotherOverLists)
                text.append("    v"_s, value, " = Integer integer=1\n    v"_s, value + 1, " = NewArray v"_s, value, ", v"_s, value, " contents=anything\n    v"_s, value + 2, " = IteratorOpen v"_s, value + 1, '\n');
            else
                text.append("    v"_s, value, " = GetModuleVariable variable=g"_s, level, "\n    v"_s, value + 1, " = Call v"_s, value, ", -, v0\n    v"_s, value + 2, " = IteratorOpen v"_s, value + 1, '\n');
            text.append("    Jump -> next b"_s, 3 * level + 1, "(v"_s, level ? value - 5 : 0, ")\n  b"_s, 3 * level + 1, "(v"_s, value + 3, ": number)"_s);
            appendHandlerAround(level);
            text.append(":\n    LoopHint\n    v"_s, value + 5, " = IteratorResume v"_s, value + 2, "\n    v"_s, value + 6, " = ResultAt v"_s, value + 5, "\n    v"_s, value + 7, " = ResultAt v"_s, value + 5, " index=1\n"_s);
            text.append("    Branch v"_s, value + 6, " -> taken b"_s, 3 * level + 2, "(), notTaken b"_s, 3 * level + 3, "()\n  b"_s, 3 * level + 2, "()"_s);
            appendHandlerAround(level);
            if (level)
                text.append(":\n    Jump -> next b"_s, 3 * level - 2, "(v"_s, value + 3, ")\n"_s);
            else
                text.append(":\n    Return v"_s, value + 3, '\n');
            text.append("  b"_s, 3 * level + 3, "()"_s);
            appendHandlerAround(level + 1);
            text.append(":\n"_s);
        }
        text.append("    v"_s, value, " = BitXor v"_s, value - 5, ", v0\n    Jump -> next b"_s, 3 * size - 2, "(v"_s, value, ")\n"_s);
        ++value;
        for (unsigned level = size; level--; value += 2) {
            text.append("  b"_s, cleanupOf(level), "() role=IterationCleanup subject=v"_s, 8 * level + 3);
            appendHandlerAround(level);
            text.append(":\n    v"_s, value, " = CatchException\n    IteratorClose v"_s, 8 * level + 3, " completion=Throw\n    Rethrow v"_s, value, '\n');
        }
        text.append("}\n"_s);
        for (unsigned generator = 0; generator < size; ++generator)
            appendGeneratorOfYieldsInRowForGrowthTests(text, generator + 2, 3);
        break;
    }
    }
    return text.toString();
}

template<LoopsOverGeneratorsForGrowthTests kind, unsigned size>
static void testWalkGrowsAsLoopsOverGeneratorsDo()
{
    InstructionCounts::begin();
    std::optional<uint64_t> smaller = instructionsOfWalkForGrowthTests(loopsOverGeneratorsForGrowthTests(kind, size), FunctionID(1u));
    std::optional<uint64_t> large = instructionsOfWalkForGrowthTests(loopsOverGeneratorsForGrowthTests(kind, 4 * size), FunctionID(1u));
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

enum class ReadInRowForGrowthTests : uint8_t {
    OfOneOfThreeElements,
    OfElementOfItsOwn,
};

static String earlyReturnsInRowForGrowthTests(ReadInRowForGrowthTests kind, unsigned numberOfStatements)
{
    StringBuilder text;
    text.append("layout L0 kind=Instance name=\"Item\" module=m0 constructor=f2 fields=[{key=\"skip\" declared={type=boolean} flags=[IsRequired]}, {key=\"weight\" declared={type=number} flags=[IsRequired]}] ownFieldCount=2\n"_s);
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\" body=f0 namespaceLayout=L(m0)\nentry module=m0\n"_s);
    text.append("function f0 module=m0 kind=Module flags=[IsChecked] thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = NewClosure function=f1\n    v1 = NewClosure function=f2\n"_s);
    text.append("    v2 = NewClass v1 layout=L0\n    v3 = FinishClass v2 layout=L0\n    v4 = Boolean\n    v5 = Integer integer=2\n    v6 = Construct v3, -, v4, v5\n"_s);
    text.append("    v7 = Builtin builtin=JSON\n    v8 = PropGet v7 key=\"parse\"\n    v9 = String string=\"{}\"\n    v10 = Call v8, v7, v9 calleeKey=\"parse\"\n"_s);
    text.append("    v11 = NewArray v6, v10, v6 contents=undefined|null|L0\n    v12 = Integer\n    Jump -> next b1(v12)\n"_s);
    text.append("  b1(v13: number):\n    LoopHint\n    v15 = Integer integer=300\n    v16 = Less v13, v15\n    Branch v16 -> taken b2(), notTaken b3()\n"_s);
    text.append("  b2():\n    v17 = Call v0, -, v11\n    v18 = Increment v13\n    Jump -> next b1(v18)\n  b3():\n    Return v13\n}\n"_s);
    text.append("function f1 name=\"run\" module=m0 flags=[IsChecked] length=1 thisType={type=anything} parameters=[{type=undefined|null|Array<undefined|null|L0> flags=[OmitsNull, OmitsUndefined]}] results=[{type=anything}] {\n"_s);
    text.append("  b0(v0: undefined|null|Array<undefined|null|L0>):\n"_s);
    unsigned value = 1;
    unsigned block = 1;
    Vector<unsigned> kept;
    for (unsigned statement = 0; statement < numberOfStatements; ++statement) {
        text.append("    v"_s, value, " = Integer integer="_s, kind == ReadInRowForGrowthTests::OfElementOfItsOwn ? statement : statement % 3, '\n');
        text.append("    v"_s, value + 1, " = ElemGet v0, v"_s, value, "\n    v"_s, value + 2, " = PropGet v"_s, value + 1, " key=\"skip\"\n    v"_s, value + 3, " = ToBoolean v"_s, value + 2, '\n');
        text.append("    Branch v"_s, value + 3, " -> taken b"_s, block, "(), notTaken b"_s, block + 1, "()\n  b"_s, block, "():\n    Return v"_s, value, "\n  b"_s, block + 1, "():\n"_s);
        text.append("    v"_s, value + 4, " = PropGet v"_s, value + 1, " key=\"weight\"\n"_s);
        kept.append(value + 4);
        value += 5;
        block += 2;
    }
    text.append("    v"_s, value, " = NewArray "_s);
    for (unsigned place = 0; place < kept.size(); ++place)
        text.append(place ? ", v"_s : "v"_s, kept[place]);
    text.append(" contents=anything\n    Return v"_s, value, "\n}\n"_s);
    text.append("function f2 name=\"Item\" module=m0 kind=BaseConstructor flags=[HasNewTarget, IsChecked, HasExplicitName] length=2 thisType={type=anything} parameters=[{type=boolean}, {type=number}] results=[{type=object flags=[IsFromCompiler]}] constructedClass=L0 {\n"_s);
    text.append("  b0(v0: anything, v1: boolean, v2: number):\n    v3 = GetConstructPrototype v0 layout=L0\n    v4 = NewObject v3 layout=L0\n    PropSet v4, v1 key=\"skip\"\n    PropSet v4, v2 key=\"weight\"\n    Return v4\n}\n"_s);
    return text.toString();
}

template<ReadInRowForGrowthTests kind, unsigned numberOfStatements>
static void testWalkGrowsAsEarlyReturnsThatEachLeaveValueAliveDo()
{
    InstructionCounts::begin();
    std::optional<uint64_t> smaller = instructionsOfWalkForGrowthTests(earlyReturnsInRowForGrowthTests(kind, numberOfStatements), FunctionID(1u));
    std::optional<uint64_t> large = instructionsOfWalkForGrowthTests(earlyReturnsInRowForGrowthTests(kind, 4 * numberOfStatements), FunctionID(1u));
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

enum class StatementInRowForGrowthTests : uint8_t {
    TestThatLeaves,
    Loop,
    UnderOneHandler,
};

static String valuesReadAtEndForGrowthTests(StatementInRowForGrowthTests kind, unsigned numberOfStatements)
{
    StringBuilder text;
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\"\n"_s);
    text.append("function f0 name=\"run\" module=m0 length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=anything}] {\n  b0(v0: anything):\n"_s);
    bool isUnderHandler = kind == StatementInRowForGrowthTests::UnderOneHandler;
    unsigned handler = 2 * numberOfStatements + 2;
    auto head = [&](unsigned block) {
        text.append("  b"_s, block, "()"_s);
        if (isUnderHandler)
            text.append(" handler=b"_s, handler, "()"_s);
        text.append(":\n"_s);
    };
    unsigned value = 1;
    unsigned block = 1;
    Vector<unsigned> made;
    if (isUnderHandler) {
        text.append("    Jump -> next b1()\n"_s);
        head(block++);
    }
    for (unsigned statement = 0; statement < numberOfStatements; ++statement) {
        made.append(value);
        text.append("    v"_s, value, " = Call v0\n    v"_s, value + 1, " = ToBoolean v"_s, value, '\n');
        switch (kind) {
        case StatementInRowForGrowthTests::TestThatLeaves:
            text.append("    Branch v"_s, value + 1, " -> taken b"_s, block + 1, "(), notTaken b"_s, block, "()\n"_s);
            head(block);
            text.append("    Throw v"_s, value, '\n');
            head(block + 1);
            break;
        case StatementInRowForGrowthTests::Loop:
            text.append("    Jump -> next b"_s, block, "()\n"_s);
            head(block);
            text.append("    LoopHint\n    Branch v"_s, value + 1, " -> taken b"_s, block, "(), notTaken b"_s, block + 1, "()\n"_s);
            head(block + 1);
            ++value;
            break;
        case StatementInRowForGrowthTests::UnderOneHandler:
            text.append("    Branch v"_s, value + 1, " -> taken b"_s, block + 1, "(), notTaken b"_s, block, "()\n"_s);
            head(block);
            text.append("    Jump -> next b"_s, block + 1, "()\n"_s);
            head(block + 1);
            break;
        }
        value += 2;
        block += 2;
    }
    text.append("    v"_s, value, " = Call v0, -"_s);
    for (unsigned read : made)
        text.append(", v"_s, read);
    text.append("\n    Return v"_s, value, '\n');
    if (isUnderHandler)
        text.append("  b"_s, handler, "() role=CatchHandler:\n    v"_s, value + 1, " = CatchValue\n    Return v0\n"_s);
    text.append("}\n"_s);
    return text.toString();
}

static std::optional<uint64_t> instructionsOfLivenessForGrowthTests(const String& text)
{
    std::optional<uint64_t> instructions;
    runOnBuildThread([&] {
        BuildContext build(makeOptionsOfTestedProgram());
        std::unique_ptr<Program> program = parseProgram(text, build);
        AOT_TEST_CHECK(program);
        BodyIndexTable indexes(static_cast<uint32_t>(program->functions.size()), 1u);
        const BodyIndex& index = indexes.build(FunctionID(0u), program->at(FunctionID(0u)), 0);
        BodyLiveness liveness;
        instructions = instructionsOfAllThreadsIn([&] {
            liveness.compute(index);
        });
        AOT_TEST_CHECK(liveness.lastUse(ValueID(1u)).index() > index.numberOfValues() - 4);
    });
    return instructions;
}

template<StatementInRowForGrowthTests kind>
static void testLivenessGrowsAsValuesReadAtEndDo()
{
    constexpr unsigned numberOfStatements = 4000;
    InstructionCounts::begin();
    std::optional<uint64_t> smaller = instructionsOfLivenessForGrowthTests(valuesReadAtEndForGrowthTests(kind, numberOfStatements));
    std::optional<uint64_t> large = instructionsOfLivenessForGrowthTests(valuesReadAtEndForGrowthTests(kind, 4 * numberOfStatements));
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

static void testChainOfJoinedTextsCostsWhatItsLastTextHolds()
{
    constexpr uint32_t numberOfJoins = 100000;
    runOnBuildThread([&] {
        BuildContext build(makeOptionsOfTestedProgram());
        std::unique_ptr<Program> program = parseProgram(withStandInLibrary(classWithManyMethodsForGrowthTests(1)), build);
        AOT_TEST_CHECK(program);
        TypeArena arena(program->types, ArenaKeeps::UntilReset);
        StringID textOfNull = program->strings.textOfNull();
        StringID textOfTrue = program->strings.textOf(true);
        StringID textOfFalse = program->strings.textOf(false);
        StringID textOfUndefined = program->strings.textOfUndefined();
        auto spells = [&](StringID text, ASCIILiteral expected) {
            return std::ranges::equal(arena.bytesOf(text), byteCast<uint8_t>(expected.span8()));
        };

        StringID appended = textOfNull;
        StringID prepended = textOfTrue;
        StringID wrapped = textOfNull;
        for (uint32_t join = 0; join < numberOfJoins; ++join) {
            appended = arena.internJoinedTexts(appended, textOfNull);
            prepended = arena.internJoinedTexts(textOfTrue, prepended);
            wrapped = arena.internJoinedTexts(arena.internJoinedTexts(textOfFalse, wrapped), textOfUndefined);
        }
        AOT_TEST_CHECK(arena.numberOfBytesOf(appended) == 4 * (numberOfJoins + 1) && arena.numberOfBytesOf(prepended) == 4 * (numberOfJoins + 1) && arena.numberOfBytesOf(wrapped) == 4 + 14 * numberOfJoins);
        AOT_TEST_CHECK(!arena.isWideText(appended));
        for (auto [text, part] : { std::pair { appended, textOfNull }, std::pair { prepended, textOfTrue } }) {
            std::span<const uint8_t> bytes = arena.bytesOf(text);
            AOT_TEST_CHECK(bytes.size() == 4 * (numberOfJoins + 1));
            for (size_t place = 0; place < bytes.size(); place += 4)
                AOT_TEST_CHECK(std::ranges::equal(bytes.subspan(place, 4), arena.bytesOf(part)));
        }
        std::span<const uint8_t> bytesOfWrapped = arena.bytesOf(wrapped);
        AOT_TEST_CHECK(std::ranges::equal(bytesOfWrapped.subspan(5 * numberOfJoins - 5, 23), byteCast<uint8_t>("falsenullundefinedundef"_s.span8())));
        AOT_TEST_CHECK(std::ranges::equal(bytesOfWrapped.first(10), byteCast<uint8_t>("falsefalse"_s.span8())) && std::ranges::equal(bytesOfWrapped.last(9), arena.bytesOf(textOfUndefined)));

        StringID again = textOfNull;
        for (uint32_t join = 0; join < numberOfJoins; ++join)
            again = arena.internJoinedTexts(again, textOfNull);
        AOT_TEST_CHECK(again == appended);

        StringID leftFirst = arena.internJoinedTexts(arena.internJoinedTexts(textOfTrue, textOfNull), textOfFalse);
        StringID rightFirst = arena.internJoinedTexts(textOfTrue, arena.internJoinedTexts(textOfNull, textOfFalse));
        AOT_TEST_CHECK(leftFirst == rightFirst && spells(leftFirst, "truenullfalse"_s));
        AOT_TEST_CHECK(arena.internText(byteCast<uint8_t>("truenullfalse"_s.span8())) == leftFirst);
        StringID ofBytes = arena.internText(byteCast<uint8_t>("falsetrue"_s.span8()));
        AOT_TEST_CHECK(arena.internJoinedTexts(textOfFalse, textOfTrue) == ofBytes && arena.internJoinedTexts(textOfTrue, textOfFalse) != ofBytes);

        constexpr std::array<uint8_t, 4> lead { 'a', 0xed, 0xa0, 0xbd };
        constexpr std::array<uint8_t, 4> trail { 0xed, 0xb8, 0x80, 'b' };
        constexpr std::array<uint8_t, 6> pair { 'a', 0xf0, 0x9f, 0x98, 0x80, 'b' };
        StringID joinedHalves = arena.internJoinedTexts(arena.internText(lead), arena.internText(trail));
        AOT_TEST_CHECK(std::ranges::equal(arena.bytesOf(joinedHalves), pair) && arena.isWideText(joinedHalves) && arena.numberOfBytesOf(joinedHalves) == 6);
        StringID halvesApart = arena.internJoinedTexts(arena.internText(trail), arena.internText(lead));
        AOT_TEST_CHECK(arena.numberOfBytesOf(halvesApart) == 8 && arena.bytesOf(halvesApart).size() == 8 && arena.isWideText(halvesApart));
        AOT_TEST_CHECK(std::ranges::equal(arena.bytesOf(arena.internJoinedTexts(halvesApart, arena.internText(trail))).last(6), pair));

        arena.reset();
        AOT_TEST_CHECK(spells(arena.internJoinedTexts(textOfNull, textOfNull), "nullnull"_s));
    });
}

static void testTextsThatAddToOneAnotherAndAreEachReadCostWhatLastOneHolds()
{
    constexpr uint32_t numberOfJoins = 100000;
    runOnBuildThread([&] {
        BuildContext build(makeOptionsOfTestedProgram());
        std::unique_ptr<Program> program = parseProgram(withStandInLibrary(classWithManyMethodsForGrowthTests(1)), build);
        AOT_TEST_CHECK(program);
        TypeArena arena(program->types, ArenaKeeps::UntilReset);
        StringID textOfNull = program->strings.textOfNull();
        StringID textOfTrue = program->strings.textOf(true);
        StringID first = arena.internJoinedTexts(textOfNull, textOfTrue);
        arena.bytesOf(first);
        struct Row {
            StringID text;
            ptrdiff_t place { 0 };
            unsigned numberOfPlaces { 0 };
        };
        std::array<Row, 2> rows { Row { first }, Row { arena.internJoinedTexts(textOfTrue, textOfTrue) } };
        for (uint32_t join = 0; join < numberOfJoins; ++join) {
            arena.internText(byteCast<uint8_t>(String::number(join).span8()));
            for (Row& row : rows) {
                row.text = arena.internJoinedTexts(arena.internJoinedTexts(row.text, textOfNull), textOfTrue);
                std::span<const uint8_t> bytes = arena.bytesOf(row.text);
                AOT_TEST_CHECK(bytes.size() == 8 * (static_cast<size_t>(join) + 2) && std::ranges::equal(bytes.last(8), byteCast<uint8_t>("nulltrue"_s.span8())));
                ptrdiff_t place = bytes.data() - arena.bytesOf(first).data();
                row.numberOfPlaces += place != std::exchange(row.place, place);
                AOT_TEST_CHECK(row.numberOfPlaces <= 32);
            }
        }
        AOT_TEST_CHECK(std::ranges::equal(arena.bytesOf(rows[0].text).first(16), byteCast<uint8_t>("nulltruenulltrue"_s.span8())) && std::ranges::equal(arena.bytesOf(rows[1].text).first(16), byteCast<uint8_t>("truetruenulltrue"_s.span8())));

        auto spells = [&](StringID made, ASCIILiteral expected) {
            return std::ranges::equal(arena.bytesOf(made), byteCast<uint8_t>(expected.span8()));
        };
        StringID stem = arena.internJoinedTexts(textOfTrue, textOfNull);
        arena.bytesOf(stem);
        StringID left = arena.internJoinedTexts(stem, textOfTrue);
        StringID right = arena.internJoinedTexts(stem, textOfNull);
        AOT_TEST_CHECK(spells(left, "truenulltrue"_s) && arena.bytesOf(left).data() == arena.bytesOf(stem).data());
        AOT_TEST_CHECK(spells(right, "truenullnull"_s) && arena.bytesOf(right).data() != arena.bytesOf(stem).data());
        AOT_TEST_CHECK(spells(stem, "truenull"_s) && spells(left, "truenulltrue"_s) && spells(first, "nulltrue"_s));
    });
}

static String sumOfTextsForGrowthTests(unsigned numberOfTerms)
{
    StringBuilder text;
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\nentry module=m0\n"_s);
    text.append("function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = Builtin builtin=GlobalThis\n    v1 = PropGet v0 key=\"sink\" [isFreeName]\n"_s);
    text.append("    v2 = NewClosure function=f1\n    v3 = Call v1, -, v2\n    Return v3\n}\n"_s);
    text.append("function f1 name=\"run\" module=m0 thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = String string=\"ab\"\n"_s);
    for (unsigned term = 1; term < numberOfTerms; ++term)
        text.append("    v"_s, term, " = Add v"_s, term - 1, ", v0\n"_s);
    text.append("    Return v"_s, numberOfTerms - 1, "\n}\n"_s);
    return text.toString();
}

static void testWalkGrowsAsTermsOfSumOfTextsDo()
{
    constexpr unsigned numberOfTerms = 6000;
    InstructionCounts::begin();
    std::optional<uint64_t> smaller = instructionsOfWalkForGrowthTests(sumOfTextsForGrowthTests(numberOfTerms), FunctionID(1u));
    std::optional<uint64_t> large = instructionsOfWalkForGrowthTests(sumOfTextsForGrowthTests(4 * numberOfTerms), FunctionID(1u));
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

static void testWalkGrowsAsTermsOfSumOfTextsDoPastLongestTextThatIsMade()
{
    constexpr unsigned numberOfTerms = 20000;
    InstructionCounts::begin();
    std::optional<uint64_t> smaller = instructionsOfWalkForGrowthTests(sumOfTextsForGrowthTests(numberOfTerms), FunctionID(1u));
    std::optional<uint64_t> large = instructionsOfWalkForGrowthTests(sumOfTextsForGrowthTests(4 * numberOfTerms), FunctionID(1u));
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

static String graphsAskedForInOneJobForGrowthTests(unsigned numberOfGraphs, unsigned modulesOfGraph)
{
    StringBuilder text;
    text.append("module m0 path=\"/main.js\" displayPath=\"main.js\" body=f0\n"_s);
    unsigned numberOfModules = numberOfGraphs * modulesOfGraph;
    for (unsigned module = 1; module <= numberOfModules; ++module) {
        text.append("module m"_s, module, " path=\"/e"_s, module, ".js\" displayPath=\"e"_s, module, ".js\" body=f"_s, module);
        if (module % modulesOfGraph)
            text.append(" requests=[{target=m"_s, module + 1, " completes=true}]"_s);
        text.append('\n');
    }
    text.append("entry module=m0\nfunction f0 name=\"main\" kind=Module results=[{type=anything}] {\n    v0 = Builtin builtin=GlobalThis\n    v1 = PropGet v0 key=\"begin\" [isFreeName]\n    v2 = Call v1, -\n"_s);
    unsigned value = 3;
    for (unsigned graph = 0; graph < numberOfGraphs; ++graph, ++value)
        text.append("    v"_s, value, " = ImportModule module=m"_s, graph * modulesOfGraph + 1, " referrer=m0\n"_s);
    text.append("    v"_s, value, " = PropGet v0 key=\"end\" [isFreeName]\n    v"_s, value + 1, " = Call v"_s, value, ", -, v"_s, value - 1, "\n    Return\n}\n"_s);
    for (unsigned module = 1; module <= numberOfModules; ++module)
        text.append("function f"_s, module, " name=\"e"_s, module, "\" module=m"_s, module, " kind=Module results=[{type=anything}] {\n    Return\n}\n"_s);
    return text.toString();
}

static std::optional<uint64_t> instructionsOfRunForGrowthTests(const String& text)
{
    std::unique_ptr<CompileOutput> output;
    runOnBuildThread([&] {
        BuildContext build(makeOptionsOfTestedProgram());
        std::unique_ptr<Program> program = parseProgram(withStandInLibrary(text), build);
        AOT_TEST_CHECK(program);
        output = compileWithMechanisms(WTF::move(program), build, mechanismsOfWay(""_s));
        AOT_TEST_CHECK(output->hasImage() && !output->section(Wire::ResultSection::EntryModuleIds).empty());
    });
    TemporaryFile image(".image"_s, output->section(Wire::ResultSection::Image));
    Vector<String> arguments {
        makeString("--aotImagePath="_s, image.path()),
        String("-e"_s),
        makeString("let atBegin; globalThis.begin = () => { atBegin = aotInstructionsOfThread(); }; globalThis.end = last => last.then(() => print(atBegin === null ? \"none\" : aotInstructionsOfThread() - atBegin)); runCompiledEntryPoint("_s, output->records<uint32_t>(Wire::ResultSection::EntryModuleIds)[0], ");"_s),
    };
    ChildProcessResult result = ChildProcessLauncher(shellPath(), { }).run(arguments.span());
    if (!result.exitedWith(EXIT_SUCCESS))
        dataLogLn(result.description(), "\n", result.output);
    AOT_TEST_CHECK(result.exitedWith(EXIT_SUCCESS));
    StringView printed = StringView(result.output).trim(isASCIIWhitespace<char16_t>);
    if (printed == "none"_s)
        return std::nullopt;
    std::optional<uint64_t> instructions = parseInteger<uint64_t>(printed);
    AOT_TEST_CHECK(instructions);
    return instructions;
}

static void testRunGrowsAsGraphsAskedForInOneJobDo()
{
    constexpr unsigned numberOfGraphs = 1000;
    std::optional<uint64_t> smaller = instructionsOfRunForGrowthTests(graphsAskedForInOneJobForGrowthTests(numberOfGraphs, 1));
    std::optional<uint64_t> large = instructionsOfRunForGrowthTests(graphsAskedForInOneJobForGrowthTests(4 * numberOfGraphs, 1));
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

static void testRunGrowsAsGraphsOfSeveralLevelsAskedForInOneJobDo()
{
    constexpr unsigned numberOfGraphs = 1000;
    std::optional<uint64_t> smaller = instructionsOfRunForGrowthTests(graphsAskedForInOneJobForGrowthTests(numberOfGraphs, 3));
    std::optional<uint64_t> large = instructionsOfRunForGrowthTests(graphsAskedForInOneJobForGrowthTests(4 * numberOfGraphs, 3));
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

static String awaitsBehindLongListForGrowthTests(unsigned numberOfClosures, unsigned numberListedAtOnce)
{
    StringBuilder text;
    text.append("module m0 path=\"/main.js\" displayPath=\"main.js\" body=f0\nentry module=m0\n"_s);
    text.append("function f0 name=\"main\" kind=Module results=[{type=anything}] {\n    v0 = Builtin builtin=GlobalThis\n    v1 = PropGet v0 key=\"begin\" [isFreeName]\n    v2 = Call v1, -\n"_s);
    text.append("    v3 = NewClosure function=f1\n    v4 = Call v3, -\n    v5 = Call v3, -\n    v6 = PropGet v0 key=\"end\" [isFreeName]\n    v7 = Call v6, -, v5\n    Return\n}\n"_s);
    text.append("function f1 name=\"run\" module=m0 flags=[IsAsync] thisType={type=anything} results=[{type=anything}] {\n  b0():\n"_s);
    unsigned next = 0;
    for (; next < numberListedAtOnce; ++next)
        text.append("    v"_s, next, " = NewClosure function=f2\n"_s);
    unsigned list = next++;
    text.append("    v"_s, list, " = NewArray v0"_s);
    for (unsigned closure = 1; closure < numberListedAtOnce; ++closure)
        text.append(", v"_s, closure);
    text.append(" contents=anything\n"_s);
    for (unsigned closure = numberListedAtOnce; closure < numberOfClosures; ++closure, next += 2)
        text.append("    v"_s, next, " = NewClosure function=f2\n    AppendElement v"_s, list, ", v"_s, next, "\n"_s);
    text.append("    v"_s, next, " = Integer\n    Jump -> next b1(v"_s, next, ")\n"_s);
    text.append("  b1(v"_s, next + 1, ": number):\n    LoopHint\n    v"_s, next + 3, " = Integer integer="_s, numberOfClosures, "\n    v"_s, next + 4, " = Less v"_s, next + 1, ", v"_s, next + 3, "\n"_s);
    text.append("    Branch v"_s, next + 4, " -> taken b2(), notTaken b3()\n  b2():\n    v"_s, next + 5, " = ElemGet v"_s, list, ", v"_s, next + 1, "\n    v"_s, next + 6, " = Call v"_s, next + 5, ", -\n"_s);
    text.append("    v"_s, next + 7, " = Null\n    v"_s, next + 8, " = Await v"_s, next + 7, "\n    v"_s, next + 9, " = Increment v"_s, next + 1, "\n    Jump -> next b1(v"_s, next + 9, ")\n"_s);
    text.append("  b3():\n    Return v"_s, next + 1, "\n}\n"_s);
    text.append("function f2 name=\"call\" module=m0 kind=Arrow thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = Integer\n    Return v0\n}\n"_s);
    return text.toString();
}

static void testRunGrowsAsAwaitsBehindLongListDo()
{
    constexpr unsigned numberOfClosures = 2000;
    std::optional<uint64_t> smaller = instructionsOfRunForGrowthTests(awaitsBehindLongListForGrowthTests(numberOfClosures, 32));
    std::optional<uint64_t> large = instructionsOfRunForGrowthTests(awaitsBehindLongListForGrowthTests(4 * numberOfClosures, 32));
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

static String classDefinedMemberByMemberForGrowthTests(unsigned numberOfMethods)
{
    StringBuilder text;
    text.append("layout L0 kind=Instance name=\"Wide\" module=m0 constructor=f1\nmodule m0 path=\"/main.js\" displayPath=\"main.js\" body=f0\nentry module=m0\n"_s);
    text.append("function f0 name=\"main\" kind=Module results=[{type=anything}] {\n    v0 = Builtin builtin=GlobalThis\n    v1 = PropGet v0 key=\"begin\" [isFreeName]\n    v2 = Call v1, -\n"_s);
    text.append("    v3 = NewClosure function=f1\n    v4 = NewClass v3 layout=L0\n"_s);
    unsigned value = 5;
    for (unsigned method = 0; method < numberOfMethods; ++method, value += 2) {
        text.append("    v"_s, value, " = NewClosure function=f"_s, method + 3, '\n');
        text.append("    DefineMethod v4, v"_s, value, " layout=L0 index="_s, method, " key=\"m"_s, method, "\"\n"_s);
    }
    text.append("    v"_s, value, " = FinishClass v4 layout=L0\n    v"_s, value + 1, " = NewClosure function=f2\n    v"_s, value + 2, " = Call v"_s, value + 1, ", -\n"_s);
    text.append("    v"_s, value + 3, " = PropGet v0 key=\"end\" [isFreeName]\n    v"_s, value + 4, " = Call v"_s, value + 3, ", -, v"_s, value + 2, "\n    Return\n}\n"_s);
    text.append("function f1 name=\"Wide\" module=m0 kind=BaseConstructor flags=[HasNewTarget, HasExplicitName] thisType={type=anything} results=[{type=object flags=[IsFromCompiler]}] constructedClass=L0 {\n"_s);
    text.append("  b0(v0: anything):\n    v1 = GetConstructPrototype v0 layout=L0\n    v2 = NewObject v1 layout=L0\n    Return v2\n}\n"_s);
    text.append("function f2 name=\"later\" module=m0 flags=[IsAsync] thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = Integer\n    Return v0\n}\n"_s);
    for (unsigned method = 0; method < numberOfMethods; ++method)
        text.append("function f"_s, method + 3, " module=m0 kind=Method thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = Integer integer=1\n    Return v0\n}\n"_s);
    return text.toString();
}

static void testRunGrowsAsMembersThatClassDefinesOneByOneDo()
{
    constexpr unsigned numberOfMethods = 1000;
    std::optional<uint64_t> smaller = instructionsOfRunForGrowthTests(classDefinedMemberByMemberForGrowthTests(numberOfMethods));
    std::optional<uint64_t> large = instructionsOfRunForGrowthTests(classDefinedMemberByMemberForGrowthTests(4 * numberOfMethods));
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

static void testResumptionCostsWhatWasKeptHoweverLargeFrameIs()
{
    constexpr unsigned numberOfClosures = 2000;
    std::optional<uint64_t> smaller = instructionsOfRunForGrowthTests(awaitsBehindLongListForGrowthTests(numberOfClosures, numberOfClosures));
    std::optional<uint64_t> large = instructionsOfRunForGrowthTests(awaitsBehindLongListForGrowthTests(4 * numberOfClosures, 4 * numberOfClosures));
    AOT_TEST_CHECK(smaller && large);
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

enum class NamesReadOffJoinsForGrowthTests : uint8_t {
    TwoBehindLastJoin,
    OneOfItsOwnInFrontOfEachJoin,
};

static String joinsInRowThatNamesAreReadOffForGrowthTests(NamesReadOffJoinsForGrowthTests kind, unsigned numberOfJoins)
{
    bool eachJoinHasName = kind == NamesReadOffJoinsForGrowthTests::OneOfItsOwnInFrontOfEachJoin;
    unsigned numberOfNames = eachJoinHasName ? numberOfJoins : 2;
    StringBuilder text;
    text.append("layout L0 kind=Instance name=\"Shape\" module=m0 constructor=f2 fields=["_s);
    for (unsigned name = 0; name < numberOfNames; ++name)
        text.append(name ? ", "_s : ""_s, "{key=\"a"_s, name, "\" declared={type=anything} flags=[IsRequired]}"_s);
    text.append("] ownFieldCount="_s, numberOfNames, '\n');
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\" body=f0 namespaceLayout=L(m0)\nentry module=m0\n"_s);
    text.append("function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {\n  b0():\n    v0 = NewClosure function=f1\n    v1 = NewClosure function=f2\n"_s);
    text.append("    v2 = NewClass v1 layout=L0\n    v3 = FinishClass v2 layout=L0\n    v4 = Integer integer=1\n    v5 = Construct v3, -, v4\n"_s);
    text.append("    v6 = Builtin builtin=JSON\n    v7 = PropGet v6 key=\"parse\"\n    v8 = String string=\"[]\"\n    v9 = Call v7, v6, v8 calleeKey=\"parse\"\n    v10 = Call v7, v6, v8 calleeKey=\"parse\"\n"_s);
    text.append("    v11 = Call v0, -, v9, v10\n    Return v11\n}\n"_s);
    text.append("function f1 name=\"pick\" module=m0 length=2 thisType={type=anything} parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {\n"_s);
    text.append("  b0(v0: anything, v1: anything):\n    v2 = Integer\n    v3 = ElemGet v1, v2\n    Jump -> next b1(v3)\n"_s);
    unsigned value = 4;
    unsigned block = 1;
    for (unsigned join = 0; join < numberOfJoins; ++join) {
        unsigned joined = value++;
        text.append("  b"_s, block, "(v"_s, joined, ": anything):\n"_s);
        if (eachJoinHasName)
            text.append("    v"_s, value++, " = PropGet v"_s, joined, " key=\"a"_s, join, "\"\n"_s);
        text.append("    v"_s, value, " = Integer integer="_s, join + 1, "\n    v"_s, value + 1, " = ElemGet v0, v"_s, value, "\n    v"_s, value + 2, " = ToBoolean v"_s, value + 1, '\n');
        text.append("    Branch v"_s, value + 2, " -> taken b"_s, block + 1, "(), notTaken b"_s, block + 2, "(v"_s, joined, ")\n"_s);
        text.append("  b"_s, block + 1, "():\n    v"_s, value + 3, " = ElemGet v1, v"_s, value, "\n    Jump -> next b"_s, block + 2, "(v"_s, value + 3, ")\n"_s);
        value += 4;
        block += 2;
    }
    text.append("  b"_s, block, "(v"_s, value, ": anything):\n"_s);
    if (eachJoinHasName)
        text.append("    Return v"_s, value, "\n}\n"_s);
    else {
        text.append("    v"_s, value + 1, " = PropGet v"_s, value, " key=\"a0\"\n    v"_s, value + 2, " = PropGet v"_s, value, " key=\"a1\"\n"_s);
        text.append("    v"_s, value + 3, " = Add v"_s, value + 1, ", v"_s, value + 2, "\n    Return v"_s, value + 3, "\n}\n"_s);
    }
    text.append("function f2 name=\"Shape\" module=m0 kind=BaseConstructor flags=[HasNewTarget, HasExplicitName] length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=object flags=[IsFromCompiler]}] constructedClass=L0 {\n"_s);
    text.append("  b0(v0: anything, v1: anything):\n    v2 = GetConstructPrototype v0 layout=L0\n    v3 = NewObject v2 layout=L0\n"_s);
    for (unsigned name = 0; name < numberOfNames; ++name)
        text.append("    PropSet v3, v1 key=\"a"_s, name, "\"\n"_s);
    text.append("    Return v3\n}\n"_s);
    return text.toString();
}

static std::optional<uint64_t> instructionsUpToFirstWalkForGrowthTests(const String& text, FunctionID function)
{
    std::optional<uint64_t> instructions;
    runOnBuildThread([&] {
        BuildContext build(makeOptionsOfTestedProgram());
        std::unique_ptr<Program> parsed = parseProgram(withStandInLibrary(text), build);
        AOT_TEST_CHECK(parsed);
        StagedBuild stages(WTF::move(parsed), build);
        instructions = instructionsOfAllThreadsIn([&] {
            AOT_TEST_CHECK(stages.code(CodeUnit { function, CodeRole::Body }));
        });
    });
    return instructions;
}

template<NamesReadOffJoinsForGrowthTests kind, unsigned numberOfJoins>
static void testFirstWalkGrowsAsJoinsThatNamesAreReadOffDo()
{
    InstructionCounts::begin();
    std::optional<uint64_t> smaller = instructionsUpToFirstWalkForGrowthTests(joinsInRowThatNamesAreReadOffForGrowthTests(kind, numberOfJoins), FunctionID(1u));
    std::optional<uint64_t> large = instructionsUpToFirstWalkForGrowthTests(joinsInRowThatNamesAreReadOffForGrowthTests(kind, 4 * numberOfJoins), FunctionID(1u));
    AOT_TEST_CHECK(growsAsSizeDoes(smaller, large));
}

std::span<const NativeTest> growthOfWorkTests()
{
    static constexpr NativeTest tests[] {
        { "growth-of-work/each-of-many-members-of-a-class-is-found-by-its-key"_s, testEachOfManyMembersIsFoundByItsKey },
        { "growth-of-work/members-of-classes-far-above-are-found-by-their-keys"_s, testMembersOfClassesFarAboveAreFoundByTheirKeys },
        { "growth-of-work/asking-each-class-object-of-a-row-what-a-lookup-does-not-find-grows-as-the-row-does"_s, testAskingEachClassObjectOfRowWhatLookupDoesNotFindGrowsAsRowDoes },
        { "growth-of-work/a-member-above-a-long-row-is-the-one-that-a-walk-up-the-row-finds"_s, testMemberAboveLongRowIsTheOneThatWalkUpRowFinds },
        { "growth-of-work/each-question-inside-what-a-lookup-did-not-find-answers-for-itself"_s, testEachQuestionInsideWhatLookupDidNotFindAnswersForItself },
        { "growth-of-work/each-of-many-effects-is-kept-once-and-stays-where-it-lies"_s, testEachOfManyEffectsIsKeptOnceAndStaysWhereItLies },
        { "growth-of-work/the-plan-grows-as-the-members-of-a-class-do"_s, testPlanGrowsAsMembersOfClassDo },
        { "growth-of-work/the-plan-grows-as-the-classes-in-a-row-of-which-the-foot-is-constructed-do"_s, testPlanGrowsAsClassesInRowDo },
        { "growth-of-work/a-function-with-more-parameters-than-a-convention-counts-is-refused"_s, testFunctionWithMoreParametersThanConventionCountsIsRefused },
        { "growth-of-work/the-facts-grow-as-the-classes-below-one-class-do"_s, testFactsGrowAsClassesBelowOneClassDo },
        { "growth-of-work/the-facts-grow-as-the-objects-created-below-one-literal-do"_s, testFactsGrowAsObjectsCreatedBelowOneLiteralDo },
        { "growth-of-work/the-walk-grows-as-the-loops-in-a-row-do"_s, testWalkGrowsAsLoopsInRowDo },
        { "growth-of-work/the-walk-grows-as-the-loops-in-a-row-that-hand-a-sum-on-inside-a-loop-do"_s, testWalkGrowsAsLoopsInRowThatHandSumOnInsideLoopDo },
        { "growth-of-work/the-walk-grows-as-the-values-that-nobody-knows-and-a-loop-reckons-with-do"_s, testWalkGrowsAsUnknownValuesReckonedWithInLoopDo },
        { "growth-of-work/the-walk-grows-as-the-names-of-a-loop-that-each-take-what-many-others-meet-in-do"_s, testWalkGrowsAsNamesOfLoopThatEachTakeWhatManyOthersMeetInDo },
        { "growth-of-work/the-walk-grows-as-the-names-of-a-loop-that-may-each-take-from-both-neighbors-do"_s, testWalkGrowsAsNamesOfLoopThatMayEachTakeFromBothNeighborsDo },
        { "growth-of-work/the-walk-grows-as-the-loops-up-to-one-bound-do"_s, testWalkGrowsAsLoopsUpToOneBoundDo },
        { "growth-of-work/a-nest-of-loops-is-walked-alike-whatever-room-its-stack-has"_s, testNestOfLoopsIsWalkedAlikeWhateverRoomStackHas },
        { "growth-of-work/a-nest-round-a-sum-that-is-deeper-than-tries-go-is-walked-little-more-than-one-that-is-not"_s, testNestDeeperThanTriesGoIsWalkedLittleMoreThanOneThatIsNot },
        { "growth-of-work/a-nest-of-loops-over-holders-of-their-own-is-walked-no-more-than-it-was"_s, testNestOfLoopsOverHoldersOfTheirOwnIsWalkedNoMoreThanItWas },
        { "growth-of-work/the-walk-of-a-loop-that-reads-many-lists-under-one-counter-grows-no-faster-than-it-did"_s, testWalkOfLoopThatReadsManyListsUnderOneCounterGrowsNoFasterThanItDid },
        { "growth-of-work/the-scratch-realm-has-its-room-wherever-a-question-stands"_s, testScratchRealmHasItsRoomWhereverQuestionStands },
        { "growth-of-work/the-walk-grows-as-the-early-returns-that-each-leave-a-value-alive-do"_s, testWalkGrowsAsEarlyReturnsThatEachLeaveValueAliveDo<ReadInRowForGrowthTests::OfOneOfThreeElements, 1000> },
        { "growth-of-work/the-walk-grows-as-the-early-returns-behind-guesses-of-their-own-that-each-leave-a-value-alive-do"_s, testWalkGrowsAsEarlyReturnsThatEachLeaveValueAliveDo<ReadInRowForGrowthTests::OfElementOfItsOwn, 150> },
        { "growth-of-work/the-liveness-of-a-body-grows-as-the-values-read-at-its-end-do"_s, testLivenessGrowsAsValuesReadAtEndDo<StatementInRowForGrowthTests::TestThatLeaves> },
        { "growth-of-work/the-liveness-of-a-body-grows-as-the-loops-that-values-live-across-do"_s, testLivenessGrowsAsValuesReadAtEndDo<StatementInRowForGrowthTests::Loop> },
        { "growth-of-work/the-liveness-of-a-body-grows-as-the-blocks-under-one-handler-do"_s, testLivenessGrowsAsValuesReadAtEndDo<StatementInRowForGrowthTests::UnderOneHandler> },
        { "growth-of-work/the-walk-grows-as-the-iterations-in-a-row-do"_s, testWalkGrowsAsIterationsInRowDo },
        { "growth-of-work/the-first-walk-of-a-body-grows-as-the-joins-in-a-row-behind-which-two-names-are-read-do"_s, testFirstWalkGrowsAsJoinsThatNamesAreReadOffDo<NamesReadOffJoinsForGrowthTests::TwoBehindLastJoin, 500> },
        { "growth-of-work/the-first-walk-of-a-body-grows-as-the-joins-in-a-row-that-each-have-a-name-of-their-own-read-in-front-do"_s, testFirstWalkGrowsAsJoinsThatNamesAreReadOffDo<NamesReadOffJoinsForGrowthTests::OneOfItsOwnInFrontOfEachJoin, 100> },
        { "growth-of-work/the-walk-grows-as-the-yields-and-the-statements-of-the-loop-that-takes-them-do"_s, testWalkGrowsAsLoopsOverGeneratorsDo<LoopsOverGeneratorsForGrowthTests::YieldsAndStatements, 100> },
        { "growth-of-work/the-walk-grows-as-the-loops-over-generators-inside-one-another-do"_s, testWalkGrowsAsLoopsOverGeneratorsDo<LoopsOverGeneratorsForGrowthTests::InsideOneAnother, 3> },
        { "growth-of-work/the-walk-grows-as-the-loops-over-lists-inside-one-another-do"_s, testWalkGrowsAsLoopsOverGeneratorsDo<LoopsOverGeneratorsForGrowthTests::InsideOneAnotherOverLists, 5> },
        { "growth-of-work/the-walk-grows-as-the-loops-over-lists-inside-one-another-do-from-twenty-on"_s, testWalkGrowsAsLoopsOverGeneratorsDo<LoopsOverGeneratorsForGrowthTests::InsideOneAnotherOverLists, 20> },
        { "growth-of-work/the-walk-grows-as-the-loops-over-generators-one-behind-another-do"_s, testWalkGrowsAsLoopsOverGeneratorsDo<LoopsOverGeneratorsForGrowthTests::OneBehindAnother, 250> },
        { "growth-of-work/the-walk-grows-as-the-generators-that-loop-over-one-another-do"_s, testWalkGrowsAsLoopsOverGeneratorsDo<LoopsOverGeneratorsForGrowthTests::GeneratorsOverOneAnother, 4> },
        { "growth-of-work/a-chain-of-joined-texts-costs-what-its-last-text-holds"_s, testChainOfJoinedTextsCostsWhatItsLastTextHolds },
        { "growth-of-work/texts-that-add-to-one-another-and-are-each-read-cost-what-the-last-one-holds"_s, testTextsThatAddToOneAnotherAndAreEachReadCostWhatLastOneHolds },
        { "growth-of-work/the-walk-grows-as-the-terms-of-a-sum-of-texts-do"_s, testWalkGrowsAsTermsOfSumOfTextsDo },
        { "growth-of-work/the-walk-grows-as-the-terms-of-a-sum-of-texts-do-past-the-longest-text-that-is-made"_s, testWalkGrowsAsTermsOfSumOfTextsDoPastLongestTextThatIsMade },
        { "growth-of-work/the-sweep-grows-as-the-births-that-each-leave-into-one-meeting-do"_s, testSweepGrowsAsBirthsThatEachLeaveIntoOneMeetingDo },
        { "growth-of-work/a-run-grows-as-the-graphs-asked-for-in-one-job-do"_s, testRunGrowsAsGraphsAskedForInOneJobDo },
        { "growth-of-work/a-run-grows-as-the-graphs-of-several-levels-asked-for-in-one-job-do"_s, testRunGrowsAsGraphsOfSeveralLevelsAskedForInOneJobDo },
        { "growth-of-work/a-run-grows-as-the-awaits-behind-a-long-list-do"_s, testRunGrowsAsAwaitsBehindLongListDo },
        { "growth-of-work/a-run-grows-as-the-members-that-a-class-defines-one-by-one-do"_s, testRunGrowsAsMembersThatClassDefinesOneByOneDo },
        { "growth-of-work/a-resumption-costs-what-was-kept-however-large-the-frame-is"_s, testResumptionCostsWhatWasKeptHoweverLargeFrameIs, "emitCopyFrameToStack() clears every word that no suspension keeps, at every resumption: a call with K arguments ahead of K awaits costs the square of K. The clearing may not go (8 of 721 programs then hold an object for ever); the resumed frame has to be shorter"_s },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
