/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSupportTestCommands.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTProvisionalFunctions.h"
#include "AOTStringArena.h"
#include "AOTThreads.h"
#include "AOTTypeArena.h"
#include "AOTTypeTable.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct NewTypeText {
    TypeID id;
    unsigned firstNew;

    void dump(PrintStream& out) const
    {
        if (!id || TypeTable::isProvisional(id) || id.index() < firstNew)
            out.print("n?"_s);
        else
            out.print('n', id.index() - firstNew);
    }
};

struct ArenaMergeChecks {
    std::atomic<bool> existingTypesStayFinal { true };
    std::atomic<bool> newTypesAreProvisional { true };
    std::atomic<bool> typesAreReadable { true };
};

using TaskTypes = std::array<TypeID, 3>;

struct InternedRange {
    unsigned maximum { 0 };
    TypeID id;
};

using InternedRanges = Vector<InternedRange, 2>;

} // anonymous namespace

static constexpr unsigned minArenaMergeTasks = 10;
static constexpr unsigned maxArenaMergeTasks = 1 << 16;

static Vector<unsigned> shuffledIndices(unsigned count, unsigned seed)
{
    Vector<unsigned> indices(count, [](size_t index) {
        return static_cast<unsigned>(index);
    });
    shuffle(indices, seed);
    return indices;
}

static void internExistingType(TypeArena& arena, ArenaMergeChecks& checks)
{
    TypeID id = arena.intern(Type::integer(Range::int32()));
    if (id != TypeTable::int32 || TypeTable::isProvisional(id))
        checks.existingTypesStayFinal = false;
}

static TaskTypes internTaskTypes(TypeArena& arena, unsigned task, ArenaMergeChecks& checks)
{
    Type sharedType = Type::integer(Range { 0, 100 + task % 8 });
    TypeID shared = arena.intern(sharedType);
    TypeID own = arena.intern(Type::integer(Range { 0, 1000 + task }));
    TypeID array = arena.intern(Type::builtinClass(BuiltinClasses::Array, singleElementSpan(shared)));
    internExistingType(arena, checks);
    TypeID repeated = arena.intern(sharedType);

    TaskTypes types { shared, own, array };
    if (!std::ranges::all_of(types, [](TypeID id) { return TypeTable::isProvisional(id); }))
        checks.newTypesAreProvisional = false;
    if (repeated != shared || arena.at(shared) != sharedType || arena.table().at(shared) != sharedType)
        checks.typesAreReadable = false;
    return types;
}

static void printNewTypes(PrintStream& out, const TypeTable& table, unsigned firstNew)
{
    for (unsigned index = firstNew; index < table.size(); ++index) {
        TypeID id(index);
        const Type& type = table.at(id);
        if (type.classes.isEmpty())
            out.println(NewTypeText { id, firstNew }, " = int["_s, type.integers.minimum, ',', type.integers.maximum, ']');
        else
            out.println(NewTypeText { id, firstNew }, " = Array<"_s, NewTypeText { type.classes[0].contentsAt(0), firstNew }, '>');
    }
}

static void runArenaMergeTasks(PrintStream& out, unsigned tasks, const CommonTestWords& words)
{
    TypeTable table(BuiltinTable::singleton());
    const unsigned firstNew = table.size();
    const unsigned lastTask = tasks - 1;
    Vector<unsigned> order = shuffledIndices(tasks, words.seed);

    ArenaMergeChecks checks;
    Vector<TaskTypes> types(tasks);
    ParallelLoops loops(words.threads);
    TypeInterningPhase phase(table, tasks);
    forEachIndexInParallel(loops, tasks, [&](unsigned index) {
        unsigned task = order[index];
        TypeArena arena(phase, task);
        if (task == lastTask)
            internExistingType(arena, checks);
        else
            types[task] = internTaskTypes(arena, task, checks);
    });
    TypeRemap remap = phase.finish(StringRemap());

    bool identityTasksMatch = !remap.isIdentity();
    for (unsigned task = 0; task < tasks; ++task) {
        if (remap.isIdentityFor(task) != (task == lastTask))
            identityTasksMatch = false;
        for (TypeID& id : types[task]) {
            remap.apply(id);
            if (TypeTable::isProvisional(id))
                checks.newTypesAreProvisional = false;
        }
    }
    if (remap(TypeTable::int32) != TypeTable::int32)
        checks.existingTypesStayFinal = false;

    out.println("new types "_s, table.size() - firstNew, " from "_s, tasks, " tasks"_s);
    printNewTypes(out, table, firstNew);
    for (unsigned task = 0; task < lastTask; ++task) {
        auto [shared, own, array] = types[task];
        out.println("task "_s, task, ": ["_s, NewTypeText { shared, firstNew }, ' ', NewTypeText { own, firstNew }, ' ', NewTypeText { array, firstNew }, ']');
    }
    out.println("task "_s, lastTask, ": [none]"_s);
    printCheck(out, "existing types stay final"_s, checks.existingTypesStayFinal);
    printCheck(out, "new types are provisional until the merge"_s, checks.newTypesAreProvisional);
    printCheck(out, "types can be read during the phase"_s, checks.typesAreReadable);
    printCheck(out, "only the last task is an identity"_s, identityTasksMatch);
}

static void internRange(TypeArena& arena, unsigned maximum, InternedRanges& ranges)
{
    ranges.append(InternedRange { maximum, arena.intern(Type::integer(Range { 0, maximum })) });
}

static bool followsTaskOrder(const TypeTable& table, unsigned firstNew, const TypeRemap& remap, std::span<const unsigned> taskOrder, std::span<const InternedRanges> rangesByTask)
{
    unsigned nextNew = firstNew;
    for (unsigned task : taskOrder) {
        for (const InternedRange& range : rangesByTask[task]) {
            TypeID id = remap(range.id);
            if (id.index() < firstNew || id.index() > nextNew || id.index() >= table.size())
                return false;
            if (table.at(id) != Type::integer(Range { 0, range.maximum }))
                return false;
            if (id.index() == nextNew)
                ++nextNew;
        }
    }
    return nextNew == table.size();
}

static void runArenaMergeOrder(PrintStream& out, const CommonTestWords& words)
{
    constexpr unsigned initialTaskCount = 64;
    constexpr unsigned addedTaskCount = 2;
    constexpr unsigned totalTaskCount = initialTaskCount * (1 + addedTaskCount);
    constexpr unsigned sharedMaximum = 3000;

    TypeTable table(BuiltinTable::singleton());
    const unsigned firstNew = table.size();
    Vector<unsigned> executionOrder = shuffledIndices(initialTaskCount, words.seed);

    Vector<InternedRanges> rangesByTask(totalTaskCount);
    Vector<unsigned> addedTasks(initialTaskCount);
    ParallelLoops loops(words.threads);
    TypeInterningPhase phase(table, initialTaskCount);
    forEachIndexInParallel(loops, initialTaskCount, [&](unsigned index) {
        unsigned task = executionOrder[index];
        TypeArena arena(phase, task);
        internRange(arena, 4000 + task, rangesByTask[task]);
        unsigned addedTask = phase.addTasks(addedTaskCount);
        addedTasks[task] = addedTask;
        internRange(arena, sharedMaximum, rangesByTask[task]);

        TypeArena addedArena(phase, addedTask);
        internRange(addedArena, sharedMaximum, rangesByTask[addedTask]);
        internRange(addedArena, 5000 + task, rangesByTask[addedTask]);
    });

    Vector<unsigned> taskOrder = executionOrder;
    taskOrder.appendVector(addedTasks);
    shuffle(taskOrder, words.seed);
    TypeRemap remap = phase.finish(taskOrder.span(), FunctionRemap(), StringRemap());

    bool unopenedTasksAreIdentities = std::ranges::all_of(addedTasks, [&](unsigned addedTask) {
        return remap.isIdentityFor(addedTask + 1);
    });
    out.println("new types "_s, table.size() - firstNew, " from "_s, taskOrder.size(), " of "_s, totalTaskCount, " tasks"_s);
    printCheck(out, "new type ids follow the given order"_s, followsTaskOrder(table, firstNew, remap, taskOrder.span(), rangesByTask.span()));
    printCheck(out, "a task that was never opened may be left out"_s, unopenedTasksAreIdentities);
}

static void runArenaDiscard(PrintStream& out)
{
    constexpr unsigned tasks = 4;
    TypeTable table(BuiltinTable::singleton());
    const unsigned firstNew = table.size();
    TypeInterningPhase phase(table, tasks);
    for (unsigned task = 0; task < tasks; ++task) {
        TypeArena arena(phase, task);
        arena.intern(Type::integer(Range { 0, 2000 + task }));
    }
    phase.discard();
    out.println("new types "_s, table.size() - firstNew, " from "_s, tasks, " tasks"_s);
}

static void runArenaSerial(PrintStream& out)
{
    TypeTable table(BuiltinTable::singleton());
    const unsigned firstNew = table.size();
    TypeArena arena(table);
    Type type = Type::integer(Range { 0, 2000 });
    TypeID first = arena.intern(type);
    TypeID second = arena.intern(type);
    out.println("new types "_s, table.size() - firstNew, " without a phase"_s);
    printCheck(out, "same id twice"_s, first == second);
    printCheck(out, "final at once"_s, !TypeTable::isProvisional(first) && table.at(first) == type);
}

void runArenaMergeTestCommand(TestCommandContext& context)
{
    if (isOnlyWord(context.arguments, "discard"_s))
        return runArenaDiscard(context.out);
    if (isOnlyWord(context.arguments, "serial"_s))
        return runArenaSerial(context.out);
    if (!context.arguments.empty() && context.arguments[0] == "order"_s) {
        if (auto words = parseCommonTestWords(context.out, context.arguments.subspan(1)))
            runArenaMergeOrder(context.out, *words);
        return;
    }

    CommonTestWords words;
    std::optional<unsigned> tasks;
    for (const String& argument : context.arguments) {
        if (takeCommonTestWord(argument, words))
            continue;
        auto number = parseTestNumber<unsigned>(argument);
        if (tasks || !number || *number < minArenaMergeTasks || *number > maxArenaMergeTasks)
            return printBadArgument(context.out, argument);
        tasks = number;
    }
    if (!tasks)
        return printMissingArgument(context.out);
    runArenaMergeTasks(context.out, *tasks, words);
}

static std::optional<Vector<uint32_t>> parseHashes(StringView word)
{
    Vector<uint32_t> hashes;
    if (word == "-"_s)
        return hashes;
    for (StringView text : word.splitAllowingEmptyEntries(',')) {
        auto hash = parseTestNumber<uint32_t>(text);
        if (!hash)
            return std::nullopt;
        hashes.append(*hash);
    }
    return hashes;
}

void runHashedIndexTestCommand(TestCommandContext& context)
{
    constexpr size_t numberOfWords = 3;

    std::span<const String> words = context.arguments;
    if (words.size() < numberOfWords)
        return printMissingArgument(context.out);
    if (words.size() > numberOfWords)
        return printBadArgument(context.out, words[numberOfWords]);
    auto hashes = parseHashes(words[0]);
    if (!hashes)
        return printBadArgument(context.out, words[0]);
    auto hash = parseTestNumber<uint32_t>(words[1]);
    if (!hash)
        return printBadArgument(context.out, words[1]);
    auto acceptedPosition = parseTestNumber<uint32_t>(words[2]);
    if (!acceptedPosition)
        return printBadArgument(context.out, words[2]);

    HashedIndex index;
    for (uint32_t position = 0; position < hashes->size(); ++position)
        index.add(hashes->at(position), position);

    unsigned numberOfComparisons = 0;
    auto found = index.find(*hash, [&](uint32_t position) {
        ++numberOfComparisons;
        return position == *acceptedPosition;
    });
    if (found)
        context.out.println("position "_s, *found, " compared "_s, numberOfComparisons);
    else
        context.out.println("none compared "_s, numberOfComparisons);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
