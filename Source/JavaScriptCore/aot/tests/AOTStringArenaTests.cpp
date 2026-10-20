/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTNameClass.h"
#include "AOTNativeTest.h"
#include "AOTStringArena.h"
#include "AOTStringTable.h"
#include "AOTThreads.h"
#include <array>
#include <optional>
#include <wtf/StdLibExtras.h>
#include <wtf/Threading.h>

namespace JSC { namespace AOT {

using Typed::ProvisionalStringID;
using Typed::StringArena;
using Typed::StringAsRead;
using Typed::StringID;
using Typed::StringInterningPhase;
using Typed::StringRemap;
using Typed::StringTable;

namespace {

std::span<const uint8_t> bytesOf(ASCIILiteral text)
{
    return byteCast<uint8_t>(text.span());
}

constexpr ASCIILiteral textAsRead = "text as read"_s;

struct Strings {
    Strings()
    {
        Vector<StringAsRead> stringsAsRead { { bytesOf(textAsRead), Typed::classifyName(bytesOf(textAsRead)), BuiltinTable::singleton().nameOf(bytesOf(textAsRead)) } };
        AOT_TEST_CHECK(table.initialize(BuiltinTable::singleton(), loops, WTF::move(stringsAsRead)).error == StringTable::Error::None);
        initialSize = table.size();
        AOT_TEST_CHECK(table.find(bytesOf(textAsRead)) == StringID(0u));
    }

    StringID idAfterInitialSize(unsigned position) const { return StringID(static_cast<uint32_t>(initialSize + position)); }
    bool holds(StringID id, ASCIILiteral text) const { return equalSpans(table[id], bytesOf(text)); }

    ParallelLoops loops { 1 };
    StringTable table;
    size_t initialSize { 0 };
};

constexpr std::array<ASCIILiteral, 4> textsOfFirstTask { "shared by both tasks"_s, "only in the first task"_s, textAsRead, "late in both"_s };
constexpr std::array<ASCIILiteral, 4> textsOfSecondTask { "only in the second task"_s, "shared by both tasks"_s, "late in both"_s, "shared by both tasks"_s };

using IDsOfTask = std::array<StringID, 4>;

struct IDsOfTasks {
    IDsOfTask first;
    IDsOfTask second;
};

enum class Schedule : uint8_t { FirstTaskFirst, SecondTaskFirst, Interleaved, InterleavedInReverse, TwoThreads };
constexpr Schedule schedules[] = { Schedule::FirstTaskFirst, Schedule::SecondTaskFirst, Schedule::Interleaved, Schedule::InterleavedInReverse, Schedule::TwoThreads };

} // anonymous namespace

static IDsOfTask internAll(StringInterningPhase& phase, unsigned task, const std::array<ASCIILiteral, 4>& texts)
{
    StringArena arena(phase, task);
    IDsOfTask ids;
    for (size_t index = 0; index < texts.size(); ++index)
        ids[index] = arena.intern(bytesOf(texts[index]));
    return ids;
}

static IDsOfTasks internInterleaved(StringInterningPhase& phase, bool secondTaskLeads)
{
    StringArena first(phase, 0);
    StringArena second(phase, 1);
    IDsOfTasks ids;
    for (size_t index = 0; index < textsOfFirstTask.size(); ++index) {
        if (secondTaskLeads)
            ids.second[index] = second.intern(bytesOf(textsOfSecondTask[index]));
        ids.first[index] = first.intern(bytesOf(textsOfFirstTask[index]));
        if (!secondTaskLeads)
            ids.second[index] = second.intern(bytesOf(textsOfSecondTask[index]));
    }
    return ids;
}

static IDsOfTasks runTasks(StringInterningPhase& phase, Schedule schedule)
{
    IDsOfTasks ids;
    switch (schedule) {
    case Schedule::FirstTaskFirst:
        ids.first = internAll(phase, 0, textsOfFirstTask);
        ids.second = internAll(phase, 1, textsOfSecondTask);
        return ids;
    case Schedule::SecondTaskFirst:
        ids.second = internAll(phase, 1, textsOfSecondTask);
        ids.first = internAll(phase, 0, textsOfFirstTask);
        return ids;
    case Schedule::Interleaved:
        return internInterleaved(phase, false);
    case Schedule::InterleavedInReverse:
        return internInterleaved(phase, true);
    case Schedule::TwoThreads: {
        Ref firstThread = Thread::create("first task"_s, [&] {
            ids.first = internAll(phase, 0, textsOfFirstTask);
        });
        Ref secondThread = Thread::create("second task"_s, [&] {
            ids.second = internAll(phase, 1, textsOfSecondTask);
        });
        firstThread->waitForCompletion();
        secondThread->waitForCompletion();
        return ids;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void checkFinalIDs(const Strings& strings, const StringRemap& remap, const IDsOfTasks& ids, std::initializer_list<ASCIILiteral> newTextsInOrder)
{
    AOT_TEST_CHECK(strings.table.size() == strings.initialSize + newTextsInOrder.size());
    unsigned position = 0;
    for (ASCIILiteral text : newTextsInOrder) {
        AOT_TEST_CHECK(strings.holds(strings.idAfterInitialSize(position), text));
        AOT_TEST_CHECK(strings.table.find(bytesOf(text)) == strings.idAfterInitialSize(position));
        ++position;
    }
    for (size_t index = 0; index < textsOfFirstTask.size(); ++index) {
        AOT_TEST_CHECK(remap(ids.first[index]) == strings.table.find(bytesOf(textsOfFirstTask[index])));
        AOT_TEST_CHECK(remap(ids.second[index]) == strings.table.find(bytesOf(textsOfSecondTask[index])));
    }
}

static void testProvisionalIDs()
{
    for (uint32_t storage : { 0u, 1u, 77u, ProvisionalStringID::maxStorages - 1 }) {
        for (uint32_t position : { 0u, 1u, 4096u, ProvisionalStringID::maxStringsPerStorage - 1 }) {
            StringID id = ProvisionalStringID::encode(storage, position);
            AOT_TEST_CHECK(ProvisionalStringID::isProvisional(id) && ProvisionalStringID::storageOf(id) == storage && ProvisionalStringID::positionOf(id) == position);
        }
    }
    AOT_TEST_CHECK(!ProvisionalStringID::isProvisional(StringID()) && !ProvisionalStringID::isProvisional(StringID(0u)) && !ProvisionalStringID::isProvisional(StringID(ProvisionalStringID::provisionalBit - 1)));
    AOT_TEST_CHECK(ProvisionalStringID::maxStorages == 2047 && ProvisionalStringID::maxStringsPerStorage == 1u << 20);
}

static void testSerialInterningReturnsExistingIDs()
{
    Strings strings;
    StringArena arena(strings.table);
    AOT_TEST_CHECK(&arena.table() == &strings.table && !strings.table.hasOpenPhase());

    AOT_TEST_CHECK(arena.intern(bytesOf(textAsRead)) == StringID(0u) && strings.table.size() == strings.initialSize);
    AOT_TEST_CHECK(arena.intern(StringView(textAsRead)) == StringID(0u) && strings.table.size() == strings.initialSize);
    AOT_TEST_CHECK(arena.intern(StringView(""_s)) == strings.table.emptyString() && strings.table.size() == strings.initialSize);

    StringID first = arena.intern(bytesOf("made by a pass"_s));
    AOT_TEST_CHECK(first == strings.idAfterInitialSize(0) && !ProvisionalStringID::isProvisional(first) && strings.table.size() == strings.initialSize + 1);
    AOT_TEST_CHECK(equalSpans(arena.at(first), bytesOf("made by a pass"_s)));
    AOT_TEST_CHECK(arena.intern(bytesOf("made by a pass"_s)) == first && arena.intern(StringView("made by a pass"_s)) == first);
    AOT_TEST_CHECK(strings.table.size() == strings.initialSize + 1);

    StringID second = arena.intern(StringView("made by another pass"_s));
    AOT_TEST_CHECK(second == strings.idAfterInitialSize(1) && StringArena(strings.table).intern(bytesOf("made by another pass"_s)) == second);

    constexpr char16_t wideText[] = { 0x4e00, 'a', 0xd800 };
    StringID wide = arena.intern(StringView(std::span<const char16_t>(wideText)));
    AOT_TEST_CHECK(wide == strings.idAfterInitialSize(2) && arena.intern(Typed::encodeProgramString(std::span<const char16_t>(wideText)).span()) == wide);
    AOT_TEST_CHECK(std::ranges::equal(Typed::decodeProgramString(arena.at(wide)), wideText));
}

static void testOpenPhaseLeavesTheTableAlone()
{
    Strings strings;
    StringInterningPhase phase(strings.table, 1);
    AOT_TEST_CHECK(strings.table.hasOpenPhase());
    {
        StringArena arena(phase, 0);
        AOT_TEST_CHECK(arena.intern(bytesOf(textAsRead)) == StringID(0u));

        StringID provisional = arena.intern(bytesOf("1.5"_s));
        AOT_TEST_CHECK(ProvisionalStringID::isProvisional(provisional));
        AOT_TEST_CHECK(arena.intern(bytesOf("1.5"_s)) == provisional && arena.intern(StringView("1.5"_s)) == provisional);
        AOT_TEST_CHECK(arena.intern(bytesOf("1.25"_s)) != provisional);
        AOT_TEST_CHECK(equalSpans(arena.at(provisional), bytesOf("1.5"_s)) && equalSpans(phase.textOf(provisional), bytesOf("1.5"_s)) && equalSpans(strings.table[provisional], bytesOf("1.5"_s)));
        AOT_TEST_CHECK(strings.table.nameClass(provisional) == Typed::classifyName(bytesOf("1.5"_s)) && strings.table.nameClass(provisional) != Typed::NameClass::Ordinary);
        AOT_TEST_CHECK(!strings.table.builtinName(provisional));
        AOT_TEST_CHECK(strings.table.size() == strings.initialSize && !strings.table.find(bytesOf("1.5"_s)));
    }
    StringRemap remap = phase.finish();
    AOT_TEST_CHECK(!strings.table.hasOpenPhase() && strings.table.size() == strings.initialSize + 2);
    AOT_TEST_CHECK(strings.table.nameClass(strings.table.find(bytesOf("1.5"_s))) == Typed::classifyName(bytesOf("1.5"_s)));
}

static void testFinalIDsDoNotDependOnTheSchedule()
{
    for (Schedule schedule : schedules) {
        Strings strings;
        StringInterningPhase phase(strings.table, 2);
        IDsOfTasks ids = runTasks(phase, schedule);
        AOT_TEST_CHECK(ids.first[2] == StringID(0u) && ids.second[1] == ids.second[3]);
        AOT_TEST_CHECK(ProvisionalStringID::isProvisional(ids.first[0]) && ProvisionalStringID::isProvisional(ids.second[0]));
        StringRemap remap = phase.finish();
        checkFinalIDs(strings, remap, ids, { "shared by both tasks"_s, "only in the first task"_s, "late in both"_s, "only in the second task"_s });
        AOT_TEST_CHECK(!remap.isIdentity() && !remap.isIdentityFor(0) && !remap.isIdentityFor(1));
    }
}

static void testFinalIDsFollowTheTaskOrder()
{
    for (Schedule schedule : schedules) {
        Strings strings;
        StringInterningPhase phase(strings.table, 2);
        IDsOfTasks ids = runTasks(phase, schedule);
        constexpr std::array<unsigned, 2> taskOrder { 1, 0 };
        StringRemap remap = phase.finish(taskOrder);
        checkFinalIDs(strings, remap, ids, { "only in the second task"_s, "shared by both tasks"_s, "late in both"_s, "only in the first task"_s });
    }
}

static void testDiscardLeavesTheTableUnchanged()
{
    Strings strings;
    {
        StringInterningPhase phase(strings.table, 2);
        runTasks(phase, Schedule::Interleaved);
        phase.discard();
    }
    AOT_TEST_CHECK(!strings.table.hasOpenPhase() && strings.table.size() == strings.initialSize);
    for (ASCIILiteral text : { "shared by both tasks"_s, "only in the first task"_s, "late in both"_s, "only in the second task"_s })
        AOT_TEST_CHECK(!strings.table.find(bytesOf(text)));
    AOT_TEST_CHECK(strings.table.find(bytesOf(textAsRead)) == StringID(0u));

    StringInterningPhase nextPhase(strings.table, 1);
    StringID provisional = internAll(nextPhase, 0, textsOfSecondTask)[0];
    StringRemap remap = nextPhase.finish();
    AOT_TEST_CHECK(remap(provisional) == strings.idAfterInitialSize(0) && strings.holds(strings.idAfterInitialSize(0), "only in the second task"_s));
}

static void testIdentity()
{
    Strings strings;
    StringID made = StringArena(strings.table).intern(bytesOf("made before the phase"_s));
    {
        StringInterningPhase phase(strings.table, 3);
        {
            StringArena arena(phase, 1);
            AOT_TEST_CHECK(arena.intern(bytesOf(textAsRead)) == StringID(0u) && arena.intern(bytesOf("made before the phase"_s)) == made);
        }
        StringRemap remap = phase.finish();
        AOT_TEST_CHECK(remap.isIdentity() && remap.isIdentityFor(0) && remap.isIdentityFor(1) && remap.isIdentityFor(2));
        AOT_TEST_CHECK(remap(made) == made && remap(StringID(0u)) == StringID(0u) && !remap(StringID()));
    }

    StringInterningPhase phase(strings.table, 4);
    StringID provisional;
    StringID provisionalOfLastTask;
    {
        StringArena findsOnly(phase, 0);
        StringArena makesOne(phase, 2);
        StringArena makesTheSame(phase, 3);
        AOT_TEST_CHECK(findsOnly.intern(bytesOf("made before the phase"_s)) == made);
        provisional = makesOne.intern(bytesOf("made in the phase"_s));
        provisionalOfLastTask = makesTheSame.intern(bytesOf("made in the phase"_s));
    }
    StringRemap remap = phase.finish();
    AOT_TEST_CHECK(!remap.isIdentity() && remap.isIdentityFor(0) && remap.isIdentityFor(1) && !remap.isIdentityFor(2) && !remap.isIdentityFor(3));
    AOT_TEST_CHECK(remap(provisional) == strings.table.find(bytesOf("made in the phase"_s)) && remap(provisionalOfLastTask) == remap(provisional));
    AOT_TEST_CHECK(remap(made) == made);

    StringID applied = provisional;
    remap.apply(applied);
    AOT_TEST_CHECK(applied == remap(provisional) && !ProvisionalStringID::isProvisional(applied));
    remap.apply(applied);
    AOT_TEST_CHECK(applied == remap(provisional));
}

static void testTaskReusingAStorageCountsItsOwnFirstUses()
{
    Strings strings;
    StringInterningPhase phase(strings.table, 2);
    StringID idInSecondTask = internAll(phase, 1, textsOfSecondTask)[0];
    StringID idInFirstTask;
    {
        StringArena arena(phase, 0);
        idInFirstTask = arena.intern(bytesOf("only in the second task"_s));
    }
    StringRemap remap = phase.finish();
    AOT_TEST_CHECK(!remap.isIdentityFor(0) && !remap.isIdentityFor(1));
    AOT_TEST_CHECK(remap(idInFirstTask) == strings.idAfterInitialSize(0) && remap(idInSecondTask) == strings.idAfterInitialSize(0));
    AOT_TEST_CHECK(strings.holds(strings.idAfterInitialSize(1), "shared by both tasks"_s) && strings.holds(strings.idAfterInitialSize(2), "late in both"_s));
}

static void testAddedTasks()
{
    Strings strings;
    StringInterningPhase phase(strings.table, 1);
    StringID late;
    StringID early;
    AOT_TEST_CHECK(phase.addTasks(2) == 1 && phase.addTasks(0) == 3 && phase.addTasks(1) == 3);
    {
        StringArena lastTask(phase, 3);
        late = lastTask.intern(bytesOf("of the last task"_s));
        StringArena addedTask(phase, 1);
        early = addedTask.intern(bytesOf("of the first added task"_s));
    }
    StringRemap remap = phase.finish();
    AOT_TEST_CHECK(remap(early) == strings.idAfterInitialSize(0) && remap(late) == strings.idAfterInitialSize(1));
    AOT_TEST_CHECK(remap.isIdentityFor(0) && !remap.isIdentityFor(1) && remap.isIdentityFor(2) && !remap.isIdentityFor(3));
}

static void testUnlistedTaskWithoutStrings()
{
    Strings strings;
    StringInterningPhase phase(strings.table, 3);
    StringID provisional;
    {
        StringArena idle(phase, 0);
        AOT_TEST_CHECK(idle.intern(bytesOf(textAsRead)) == StringID(0u));
        StringArena busy(phase, 2);
        provisional = busy.intern(bytesOf("of the listed task"_s));
    }
    constexpr std::array<unsigned, 1> taskOrder { 2 };
    StringRemap remap = phase.finish(taskOrder);
    AOT_TEST_CHECK(remap(provisional) == strings.idAfterInitialSize(0) && strings.table.size() == strings.initialSize + 1);
    AOT_TEST_CHECK(remap.isIdentityFor(0) && remap.isIdentityFor(1) && !remap.isIdentityFor(2));
}

std::span<const NativeTest> stringArenaTests()
{
    static constexpr NativeTest tests[] = {
        { "string-arena/provisional-ids"_s, testProvisionalIDs },
        { "string-arena/serial-interning-returns-existing-ids"_s, testSerialInterningReturnsExistingIDs },
        { "string-arena/open-phase-leaves-the-table-alone"_s, testOpenPhaseLeavesTheTableAlone },
        { "string-arena/final-ids-do-not-depend-on-the-schedule"_s, testFinalIDsDoNotDependOnTheSchedule },
        { "string-arena/final-ids-follow-the-task-order"_s, testFinalIDsFollowTheTaskOrder },
        { "string-arena/discard-leaves-the-table-unchanged"_s, testDiscardLeavesTheTableUnchanged },
        { "string-arena/identity"_s, testIdentity },
        { "string-arena/task-reusing-a-storage-counts-its-own-first-uses"_s, testTaskReusingAStorageCountsItsOwnFirstUses },
        { "string-arena/added-tasks"_s, testAddedTasks },
        { "string-arena/unlisted-task-without-strings"_s, testUnlistedTaskWithoutStrings },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
