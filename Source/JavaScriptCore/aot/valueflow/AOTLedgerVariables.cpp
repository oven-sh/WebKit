/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLedgerVariables.h"

#if ENABLE(AOT)

#include "AOTBlockCycles.h"
#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTClaims.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTValueFlowBuiltinTables.h"
#include "AOTValueFlowHeapTables.h"
#include "AOTValueFlowResultsUnseenCode.h"
#include "AOTValueFlowRulesModules.h"
#include "AOTValueIdentity.h"
#include <wtf/Atomics.h>
#include <wtf/EnumSet.h>

namespace JSC { namespace AOT { namespace Typed {

using Locations = LedgerOfVariables;
using WriteAt = LedgerOfVariables::WriteAt;
using ReadAt = LedgerOfVariables::ReadAt;
using ReadNoted = LedgerOfVariables::ReadNoted;
using Report = LedgerOfVariables::Report;

LedgerOfVariables::LedgerOfVariables(const Program& program)
    : m_numberOfVariables(program.variables.size())
{
    uint32_t next = program.variables.size();
    m_firstFieldOfLayout = program.layouts.map([&](const Layout& layout) {
        uint32_t first = next;
        next += layout.isEnvironment() ? layout.fields.size() : 0;
        return first;
    });
    m_leftOpenToWriters.fill(0, next);
    m_leftOpenToReaders.fill(0, next);
    m_readsNoted.fill(0, next);
    m_namespaces = Vector<NamespaceObject>(program.modules.size());
}

LedgerOfVariables::~LedgerOfVariables() = default;

template<typename Entry, typename EntryAt>
static void listByLocation(uint32_t numberOfLocations, std::initializer_list<std::span<const EntryAt>> lists, Entry EntryAt::* entry, Vector<Entry>& listed, Vector<uint32_t>& first)
{
    first.fill(0, numberOfLocations + 1);
    size_t size = 0;
    for (std::span<const EntryAt> list : lists) {
        size += list.size();
        for (const EntryAt& at : list)
            ++first[at.location + 1];
    }
    for (uint32_t location = 0; location < numberOfLocations; ++location)
        first[location + 1] += first[location];
    listed = Vector<Entry>(size);
    Vector<uint32_t> next(first.span().first(numberOfLocations));
    for (std::span<const EntryAt> list : lists) {
        for (const EntryAt& at : list)
            listed[next[at.location]++] = at.*entry;
    }
}

void LedgerOfVariables::close()
{
    m_owing = actorsThatOweReports();

    Vector<ReadAt> readsNoted;
    for (uint32_t location = 0; location < numberOfLocations(); ++location) {
        auto noted = OptionSet<ReadNoted>::fromRaw(m_readsNoted[location]);
        if (noted.contains(ReadNoted::AsValueByOperationThatNamesPlace))
            readsNoted.append({ location, { Actor::Statements, ReadForm::AsValue, { }, { }, { }, true } });
        if (noted.contains(ReadNoted::AsCalleeByOperationThatNamesPlace))
            readsNoted.append({ location, { Actor::Statements, ReadForm::AsCallee, { }, { }, { }, true } });
        if (noted.contains(ReadNoted::ByLoader))
            readsNoted.append({ location, { Actor::Loader, ReadForm::AsValue, { }, { }, { }, false } });
    }
    listByLocation(numberOfLocations(), { m_writesThatNamePlaces.span(), m_writesThroughHolders.span() }, &WriteAt::write, m_writes, m_firstWrite);
    listByLocation(numberOfLocations(), { readsNoted.span(), m_readsThroughHolders.span() }, &ReadAt::read, m_reads, m_firstRead);
    m_readsNoted = { };
    m_writesThatNamePlaces = { };
    m_writesThroughHolders = { };
    m_readsThroughHolders = { };
}

std::optional<Writers> LedgerOfVariables::writersOf(const Place& place) const
{
    switch (place.kind()) {
    case PlaceKind::Variable:
        return writersAt(of(place.variable()));
    case PlaceKind::EnvironmentField:
        return writersAt(of(place.layout(), place.field()));
    case PlaceKind::Property:
    case PlaceKind::Elements:
    case PlaceKind::EveryOwnProperty:
    case PlaceKind::Accessor:
    case PlaceKind::PrototypeLink:
    case PlaceKind::Field:
    case PlaceKind::Cell:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::optional<Readers> LedgerOfVariables::readersOf(const Place& place) const
{
    switch (place.kind()) {
    case PlaceKind::Variable:
        return readersAt(of(place.variable()));
    case PlaceKind::EnvironmentField:
        return readersAt(of(place.layout(), place.field()));
    case PlaceKind::Property:
    case PlaceKind::Elements:
    case PlaceKind::EveryOwnProperty:
    case PlaceKind::Accessor:
    case PlaceKind::PrototypeLink:
    case PlaceKind::Field:
    case PlaceKind::Cell:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Reachers LedgerOfVariables::reachersOfNamespace(ModuleID module) const
{
    static constexpr Reach madeByLoader { Actor::Loader, { } };
    const NamespaceObject& object = m_namespaces[module.index()];
    return { object.isCreated ? singleElementSpan(madeByLoader) : std::span<const Reach>(), object.open };
}

Readers LedgerOfVariables::readersOfNamespaceUnderNamesNobodyLists(ModuleID module) const
{
    static constexpr std::array<Read, numberOfActors> readBy = [] {
        std::array<Read, numberOfActors> reads;
        for (Actor actor : actorsInOrder)
            reads[indexOf(actor)].actor = actor;
        return reads;
    }();
    const NamespaceObject& object = m_namespaces[module.index()];
    bool looksWereCollected = OptionSet<Report>::fromRaw(m_heard).contains(Report::AccessesThroughHolders);
    return { object.readerUnderNamesNobodyLists ? singleElementSpan(readBy[indexOf(*object.readerUnderNamesNobodyLists)]) : std::span<const Read>(), looksWereCollected ? object.open : everyActor };
}

static std::optional<uint32_t> locationReadBy(const Locations& locations, const Value& value)
{
    switch (value.opcode) {
    case Opcode::GetModuleVariable:
        return locations.of(value.variable());
    case Opcode::GetEnvironmentField:
        return locations.of(value.layout(), value.field());
    default:
        return std::nullopt;
    }
}

static VariableID exportsVariableReadBy(const Program& program, const Value& value)
{
    switch (value.opcode) {
    case Opcode::RequireModule:
    case Opcode::ImportModule:
    case Opcode::ModuleNamespace: {
        const Module& module = program.at(value.module());
        ModuleID captured = commonJsModuleCapturedBy(program, module);
        return captured ? program.at(captured).exportsVariable : module.exportsVariable;
    }
    default:
        return { };
    }
}

static void forEachLocationStoredBy(const Program& program, const Locations& locations, const Value& value, NOESCAPE const Invocable<void(uint32_t)> auto& functor)
{
    switch (value.opcode) {
    case Opcode::PutModuleVariable:
        functor(locations.of(value.variable()));
        return;
    case Opcode::PutEnvironmentField:
        functor(locations.of(value.layout(), value.field()));
        return;
    case Opcode::NewObject:
        if (!program.at(value.layout()).isEnvironment())
            return;
        for (uint32_t field : value.fields())
            functor(locations.of(value.layout(), field));
        return;
    default:
        return;
    }
}

template<typename ReadSite>
static void forEachEventOfReads(const Function& function, std::span<const ReadSite> reads, const auto& siteOf, const auto& note)
{
    if (function.role == FunctionRole::Model) {
        for (const ReadSite& read : reads)
            note(read, ReadForm::AsValue);
        return;
    }

    Vector<ValueID, 128> readBehind(function.values.size());
    BitVector isUsed(function.values.size());
    BitVector isReadAsValue(function.values.size());
    Vector<ValueID, 16> casts;
    for (const ReadSite& read : reads)
        readBehind[siteOf(read).index()] = siteOf(read);
    auto noteUsesAsValues = [&](std::span<const ValueID> used) {
        for (ValueID value : used) {
            if (ValueID read = value ? readBehind[value.index()] : ValueID())
                isReadAsValue.quickSet(read.index());
        }
    };
    auto visit = [&](const Block& block) {
        if (block.subject)
            noteUsesAsValues(singleElementSpan(block.subject));
        if (block.handler)
            noteUsesAsValues(block.handler->arguments.span());
        for (ValueID user : block.values) {
            Opcode opcode = function.at(user).opcode;
            std::span<const ValueID> operands = function.at(user).operands();
            for (uint32_t index = 0; index < operands.size(); ++index) {
                ValueID read = operands[index] ? readBehind[operands[index].index()] : ValueID();
                if (!read)
                    continue;
                isUsed.quickSet(operands[index].index());
                OperandUses operandUses = operandUsesOf(opcode, index);
                if (isCast(opcode) && operandUses.contains(OperandUse::Returns)) {
                    RELEASE_ASSERT(!readBehind[user.index()] || readBehind[user.index()] == read);
                    if (!readBehind[user.index()])
                        casts.append(user);
                    readBehind[user.index()] = read;
                    continue;
                }
                if (!operandUses.contains(OperandUse::Calls) || operandUses.containsAny({ OperandUse::Retains, OperandUse::Returns }))
                    isReadAsValue.quickSet(read.index());
            }
        }
        noteUsesAsValues(block.terminator.operands());
        for (const Target& target : block.terminator.targets)
            noteUsesAsValues(target.arguments.span());
    };
    if (function.blocks.size() == 1)
        visit(function.blocks[0]);
    else {
        CFG cfg(function);
        for (BlockID block : cfg.reversePostOrder())
            visit(function.at(block));
    }
    for (ValueID cast : casts) {
        if (!isUsed.quickGet(cast.index()))
            isReadAsValue.quickSet(readBehind[cast.index()].index());
    }
    for (const ReadSite& read : reads)
        note(read, isReadAsValue.quickGet(siteOf(read).index()) || !isUsed.quickGet(siteOf(read).index()) ? ReadForm::AsValue : ReadForm::AsCallee);
}

static ReadNoted notedFor(ReadForm form)
{
    return form == ReadForm::AsCallee ? ReadNoted::AsCalleeByOperationThatNamesPlace : ReadNoted::AsValueByOperationThatNamesPlace;
}

static void reportOperationsOf(const Program& program, FunctionID id, ExecutionCount executions, Locations& locations, Vector<WriteAt>& writes)
{
    const Function& function = program.at(id);
    Vector<ValueID, 32> reads;
    size_t firstWrite = writes.size();
    Vector<uint32_t, 16> blocksOfWrites;
    for (uint32_t block = 0; block < function.blocks.size(); ++block) {
        for (ValueID site : function.blocks[block].values) {
            const Value& value = function.at(site);
            if (locationReadBy(locations, value))
                reads.append(site);
            if (VariableID exports = exportsVariableReadBy(program, value))
                locations.note(locations.of(exports), ReadNoted::ByLoader);
            if (value.opcode == Opcode::ModuleNamespace || value.opcode == Opcode::ImportModule)
                locations.noteOperationNamingNamespaceOf(value.module());
            bool isCreation = value.opcode == Opcode::NewObject;
            forEachLocationStoredBy(program, locations, value, [&](uint32_t location) {
                writes.append({ location, { isCreation ? Actor::Creation : Actor::Statements, isCreation ? WriteForm::AtCreation : WriteForm::Store, { }, { }, id, site, traitsOfOperationThatNamesPlace } });
                blocksOfWrites.append(block);
            });
        }
    }
    if (executions == ExecutionCount::AtMostOnce && !blocksOfWrites.isEmpty()) {
        CFG cfg(function);
        BlockCycles cycles(cfg);
        BitVector runsAtMostOnce(function.blocks.size());
        for (BlockID block : cfg.reversePostOrder())
            runsAtMostOnce.quickSet(block.index(), !cycles.isInCycle(block));
        for (size_t index = 0; index < blocksOfWrites.size(); ++index) {
            if (runsAtMostOnce.quickGet(blocksOfWrites[index]))
                writes[firstWrite + index].write.traits.add(WriteTrait::RunsAtMostOnce);
        }
    }
    if (reads.isEmpty())
        return;
    forEachEventOfReads(function, reads.span(), std::identity { }, [&](ValueID read, ReadForm form) {
        locations.note(*locationReadBy(locations, function.at(read)), notedFor(form));
    });
}

static void reportBodyWithoutSolve(const Program& program, const Function& function, Locations& locations)
{
    for (const Value& value : function.values) {
        if (std::optional<uint32_t> location = locationReadBy(locations, value))
            locations.leaveOpenToReader(*location, Actor::UnseenCode);
        if (VariableID exports = exportsVariableReadBy(program, value))
            locations.note(locations.of(exports), ReadNoted::ByLoader);
        forEachLocationStoredBy(program, locations, value, [&](uint32_t location) {
            locations.leaveOpenToWriter(location, Actor::UnseenCode);
        });
    }
}

void reportOperationsOnVariables(const ValueFlowResultInputs& inputs, const FlowResultTables& tables, LedgerOfVariables& locations)
{
    const Program& program = inputs.program;
    locations.addWritesThatNamePlaces(collectInParallel<WriteAt>(inputs.loops(), program.functions.size(), [&](unsigned index, Vector<WriteAt>& writes) {
        FunctionID id(index);
        if (isUnsolvedCode(inputs, tables, id))
            reportBodyWithoutSolve(program, program.functions[index], locations);
        else if (inputs.isReached(id))
            reportOperationsOf(program, id, tables.functions.functions[index].executions, locations, writes);
    }));
    locations.didHear(Report::OperationsThatNamePlaces);
    locations.didHear(Report::BodiesWithoutSolve);
}

// Evaluated text finds a name only through the bindings of its scope.
static void reportScopesOfEvaluatedText(const Program& program, Locations& locations)
{
    for (const EvalScope& scope : program.evalScopes) {
        for (const EvalBinding& binding : scope.bindings) {
            if (binding.variable)
                locations.leaveOpen(locations.of(binding.variable), Actor::UnseenCode);
            if (binding.layout && binding.field && program.at(binding.layout).isEnvironment())
                locations.leaveOpen(locations.of(binding.layout, *binding.field), Actor::UnseenCode);
        }
    }
    locations.didHear(Report::ScopesOfEvaluatedText);
}

static bool fillsItsExportsItself(const Program& program, const Module& module)
{
    switch (module.kind) {
    case ModuleKind::SourceText:
    case ModuleKind::CommonJs:
    case ModuleKind::Data:
    case ModuleKind::RuntimeSource:
        return module.body && program.at(module.body).hasBody();
    case ModuleKind::Runtime:
    case ModuleKind::External:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The loader knows a variable only by the records of a module: its exports, its synthetic exports, the variable that holds `module.exports`. It reaches no other.
// It fills what no body of the program fills, and hands exports to whoever asks for them by name.
static void reportModules(const ValueFlowResultInputs& inputs, std::span<const ModuleFlow> modules, Locations& locations)
{
    const Program& program = inputs.program;
    BitVector hasExportsReadByName(program.modules.size());
    for (const Entry& entry : program.entries) {
        if (!entry.hostReads.isEmpty())
            hasExportsReadByName.quickSet(entry.module.index());
    }
    for (uint32_t index = 0; index < program.modules.size(); ++index) {
        const Module& module = program.modules[index];
        for (const ModuleRequest& request : module.requests) {
            if (!request.isDynamicStarExport)
                continue;
            hasExportsReadByName.quickSet(index);
            hasExportsReadByName.quickSet(request.target.index());
        }
        for (const RequiredName& requiredName : module.requiredNames)
            hasExportsReadByName.quickSet(requiredName.module.index());
    }

    BuiltinName moduleExportsName = program.builtins().nameOf(byteCast<uint8_t>("module.exports"_s.span()));
    auto isReadByRequire = [&](const ModuleExport& entry) {
        BuiltinName name = program.builtinName(entry.name);
        return name && (name == BuiltinNames::__esModule || name == moduleExportsName);
    };

    for (uint32_t index = 0; index < program.modules.size(); ++index) {
        const Module& module = program.modules[index];
        bool isFilledByLoader = !fillsItsExportsItself(program, module);
        bool isEveryExportRead = hasExportsReadByName.quickGet(index) || module.kind == ModuleKind::RuntimeSource || module.requireResult == RequireResult::DefaultExport || module.requireResult == RequireResult::NamespaceIfImported || modules[index].namespaceUse == NamespaceUse::Unknown;
        for (const ModuleExport& entry : module.exports) {
            if (entry.kind != ModuleExportKind::Variable || !entry.variable)
                continue;
            if (isEveryExportRead || (module.requireResult == RequireResult::Namespace && isReadByRequire(entry)))
                locations.note(locations.of(entry.variable), ReadNoted::ByLoader);
            if (isFilledByLoader)
                locations.leaveOpenToWriter(locations.of(entry.variable), Actor::Loader);
            if (program.at(entry.variable).flags.contains(VariableFlags::IsReplaceable))
                locations.leaveOpen(locations.of(entry.variable), Actor::NativeCode);
        }
        for (const SyntheticExport& entry : module.syntheticExports) {
            if (entry.variable)
                locations.leaveOpen(locations.of(entry.variable), entry.isReplaceable ? OptionSet<Actor> { Actor::Loader, Actor::NativeCode } : OptionSet<Actor> { Actor::Loader });
        }
        if (module.exportsVariable && (module.flags.contains(ModuleFlags::MayBeMocked) || module.flags.contains(ModuleFlags::MayBeEvicted)))
            locations.leaveOpen(locations.of(module.exportsVariable), Actor::NativeCode);
        if (ModuleID captured = commonJsModuleCapturedBy(program, module))
            locations.note(locations.of(program.at(captured).exportsVariable), ReadNoted::ByLoader);
    }

    for (size_t index : modulesThatHostAsksFor(program, filesOfProgramAreLoadedByName(inputs.solution))) {
        if (VariableID exports = program.modules[index].exportsVariable)
            locations.note(locations.of(exports), ReadNoted::ByLoader);
    }
    // Whoever holds a module object reads and stores its `exports`, and nobody follows that to the variable.
    const ValueFlowUniverse& universe = inputs.universe();
    inputs.sets().forEachMember(inputs.solver().possibleValuesOf(inputs.bodies().nodes().ofHost(ValueFlowHostNode::ModuleObjectsHandedOut)), [&](ValueFlowObjectID moduleObject) {
        ModuleID module = universe.moduleOf(moduleObject);
        if (VariableID exports = module ? program.at(module).exportsVariable : VariableID())
            locations.leaveOpen(locations.of(exports), everyActor);
    });
    locations.didHear(Report::Modules);
}

enum class ObjectAccessKind : uint8_t {
    Read,
    Observation,
    Store,
};

struct ObjectAccess {
    VariableID variable;
    FunctionID function;
    ValueID site;
    ObjectAccessKind kind { ObjectAccessKind::Observation };
    Actor actor { Actor::KeysNobodyKnows };
    WriteForm form { WriteForm::Store };
    uint8_t traits { 0 };
    ValueFlowStatementID by;

    friend auto operator<=>(const ObjectAccess&, const ObjectAccess&) = default;
};

static constexpr EnumSet<ValueFlowNameKind> namesOfValues { ValueFlowNameKind::Property, ValueFlowNameKind::Elements, ValueFlowNameKind::UnknownKey, ValueFlowNameKind::EveryProperty };
static constexpr EnumSet<ValueFlowNameKind> namesOfValuesAndAccessors = namesOfValues | EnumSet<ValueFlowNameKind> { ValueFlowNameKind::Getter, ValueFlowNameKind::Setter, ValueFlowNameKind::GetterUnderUnknownKey, ValueFlowNameKind::SetterUnderUnknownKey };

static constexpr EnumSet<ValueFlowNameKind> namesOfRemovalsAndRedefinitions { ValueFlowNameKind::DeletedMember, ValueFlowNameKind::DeletedMemberUnderUnknownKey, ValueFlowNameKind::RedefinedMember, ValueFlowNameKind::RedefinedMemberUnderUnknownKey };

static ValueFlowName propertyChangedUnder(ValueFlowName written)
{
    bool namesProperty = written.kind == ValueFlowNameKind::DeletedMember || written.kind == ValueFlowNameKind::RedefinedMember;
    return namesProperty ? ValueFlowName { ValueFlowNameKind::Property, written.payload } : everyPropertyName;
}

static bool testsProperty(const BuiltinPropertyAction& action)
{
    return action.kind == BuiltinPropertyActionKind::TestsByKey || action.kind == BuiltinPropertyActionKind::ReadsGetter || action.kind == BuiltinPropertyActionKind::ReadsSetter;
}

// A class object shows its static fields and a namespace object its exports. Whoever is handed one of them reaches the variables without naming them.
struct AccessesThroughHolders {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(AccessesThroughHolders);

    struct VariableOnObject {
        ValueFlowObjectID object;
        VariableID variable;
        PropertyKeyID key;
        bool mayLieUnderAnyName { false };

        bool mayBeNamedBy(ValueFlowName name) const
        {
            if (name.kind == ValueFlowNameKind::Property)
                return mayLieUnderAnyName || (key && key.index() == name.payload);
            return name.kind == ValueFlowNameKind::Elements ? mayLieUnderAnyName : name.kind == ValueFlowNameKind::UnknownKey || name.kind == ValueFlowNameKind::EveryProperty;
        }
    };

    AccessesThroughHolders(const ValueFlowResultInputs& inputs, LedgerOfVariables& locations)
        : inputs(inputs)
        , locations(locations)
        , solver(inputs.solver())
        , sets(inputs.sets())
        , heap(inputs)
    {
    }

    static void setFlag(Vector<uint8_t>& flags, uint32_t index) { AtomicRef(flags[index]).store(1, std::memory_order_relaxed); }
    bool isNamespace(ValueFlowObjectID object) const { return inputs.universe().kindOf(object) == ValueFlowObjectKind::Namespace; }
    std::span<const VariableOnObject> variablesOn(ValueFlowObjectID object) const LIFETIME_BOUND { return std::span(std::ranges::equal_range(variables, object, { }, &VariableOnObject::object)); }
    ValueFlowName nameOf(PropertyKeyID key) const { return ValueFlowName::property(inputs.program, key); }

    // In the order of variablesOn().
    void forEachVariableNamedBy(ValueFlowObjectID object, ValueFlowName name, NOESCAPE const Invocable<void(const VariableOnObject&)> auto& functor) const
    {
        AOT_THRESHOLD(size_t, mostVariablesWalked, 16);
        std::span<const VariableOnObject> all = variablesOn(object);
        if (name.kind != ValueFlowNameKind::Property || all.size() <= mostVariablesWalked) {
            for (const VariableOnObject& variable : all) {
                if (variable.mayBeNamedBy(name))
                    functor(variable);
            }
            return;
        }
        auto placeOf = [&](uint32_t position) { return std::pair { variables[position].object, variables[position].key.index() }; };
        std::span<const uint32_t> keyed = std::span(std::ranges::equal_range(positionsByKey, std::pair { object, name.payload }, { }, placeOf));
        std::span<const uint32_t> underAnyName = std::span(std::ranges::equal_range(positionsUnderAnyName, object, { }, [&](uint32_t position) { return variables[position].object; }));
        while (!keyed.empty() || !underAnyName.empty()) {
            std::span<const uint32_t>& next = underAnyName.empty() || (!keyed.empty() && keyed.front() < underAnyName.front()) ? keyed : underAnyName;
            functor(variables[next.front()]);
            next = next.subspan(1);
        }
    }

    void indexVariables()
    {
        for (uint32_t position = 0; position < variables.size(); ++position) {
            if (variables[position].mayLieUnderAnyName)
                positionsUnderAnyName.append(position);
            else if (variables[position].key)
                positionsByKey.append(position);
        }
        AOT::stableSortInLine(positionsByKey, { }, [&](uint32_t position) { return std::pair { variables[position].object, variables[position].key.index() }; });
    }

    bool reachesFollowedObject(ValueFlowStatementID sharedLookup) const { return lookupReachesFollowedObject[sharedLookup.index() - solver.input().statements.size()]; }

    bool hasObjectIn(ValueSetID set)
    {
        uint32_t index = ValueSets::indexInTableBySet(set);
        if (index / 4 >= setsWithObject.size())
            return sets.meet(set, setOfObjects);
        AtomicRef entry(setsWithObject[index / 4]);
        unsigned shift = index % 4 * 2;
        uint8_t known = entry.load(std::memory_order_relaxed) >> shift & 3;
        if (!known) {
            known = sets.meet(set, setOfObjects) ? 2 : 1;
            entry.fetch_or(known << shift, std::memory_order_relaxed);
        }
        return known == 2;
    }

    void forEachObjectIn(ValueSetID set, NOESCAPE const Invocable<void(ValueFlowObjectID)> auto& functor)
    {
        if (!hasObjectIn(set))
            return;
        bool listsSet = !ValueSets::isComplement(set) && sets.size(set) <= objects.size();
        sets.forEachMember(listsSet ? set : setOfObjects, [&](ValueFlowObjectID object) {
            if (sets.contains(listsSet ? setOfObjects : set, object))
                functor(object);
        });
    }

    void forEachObjectStoredThrough(ValueSetID set, NOESCAPE const Invocable<void(ValueFlowObjectID, bool isListed)> auto& functor)
    {
        bool holdsUnknownObject = sets.contains(set, ValueFlowUniverse::unknownObject());
        if (holdsUnknownObject) {
            sets.forEachMember(setOfEscapedObjects, [&](ValueFlowObjectID object) {
                functor(object, false);
            });
        }
        forEachObjectIn(set, [&](ValueFlowObjectID object) {
            if (!holdsUnknownObject || !sets.contains(setOfEscapedObjects, object))
                functor(object, true);
        });
    }

    bool isSharedLookup(ValueFlowStatementID id) const
    {
        if (!solver.isMadeByRule(id) || solver.statementAt(id).kind != ValueFlowStatementKind::Lookup)
            return false;
        ValueFlowNameKind kind = solver.keyOf(id).name().kind;
        return kind == ValueFlowNameKind::Property || kind == ValueFlowNameKind::Elements || kind == ValueFlowNameKind::EveryProperty;
    }

    void findLookupsReachingFollowedObjects()
    {
        uint32_t first = solver.input().statements.size();
        uint32_t count = lookupReachesFollowedObject.size();
        forEachIndexInParallel(inputs.loops(), count, [&](unsigned index) {
            ValueFlowStatementID id(first + index);
            if (!isSharedLookup(id) || solver.isAtRest(id))
                return;
            inputs.forEachMadeNodeAskedForBy(id, kindsOfMadeNodes({ ValueFlowMadeNodeKind::Read }), false, [&](const ValueFlowNodeKey& key, ValueFlowStatementID) {
                if (namesOfValues.contains(key.name().kind) && hasObjectIn(key.set))
                    lookupReachesFollowedObject[index] = 1;
            });
        });
        Vector<uint32_t> handingOn = findIndicesInParallel(inputs.loops(), count, [&](unsigned index) {
            ValueFlowStatementID id(first + index);
            if (lookupReachesFollowedObject[index] || !isSharedLookup(id) || solver.isAtRest(id))
                return false;
            bool handsOn = false;
            inputs.forEachMadeNodeAskedForBy(id, kindsOfMadeNodes({ ValueFlowMadeNodeKind::Lookup }), false, [&](const ValueFlowNodeKey&, ValueFlowStatementID made) {
                handsOn |= made && made != id && isSharedLookup(made) && reachesFollowedObject(made);
            });
            return handsOn;
        });
        for (uint32_t index : handingOn)
            lookupReachesFollowedObject[index] = 1;
    }

    void noteLookInto(ValueFlowObjectID object, ValueFlowName name, ObjectAccess access, Vector<ObjectAccess>& found)
    {
        if (isNamespace(object) && name.kind != ValueFlowNameKind::Property) {
            setFlag(isNamespaceReadUnderUnknownName, inputs.universe().moduleOf(object).index());
            return;
        }
        if (access.by)
            access.actor = actorOf(solver.statementAt(access.by), name, true);
        forEachVariableNamedBy(object, name, [&](const VariableOnObject& variable) {
            access.variable = variable.variable;
            found.append(access);
        });
    }

    void noteLook(ValueSetID lookedInto, ValueFlowName name, ObjectAccess access, Vector<ObjectAccess>& found)
    {
        forEachObjectIn(lookedInto, [&](ValueFlowObjectID object) {
            noteLookInto(object, name, access, found);
        });
    }

    // A namespace never changes. Whether it accepts a removal or a definition depends on the export of that name and on its value.
    void noteChangeAskedOfNamespaces(ValueSetID asked, ValueFlowName written, ObjectAccess access, Vector<ObjectAccess>& found)
    {
        forEachObjectIn(asked, [&](ValueFlowObjectID object) {
            if (isNamespace(object))
                noteLookInto(object, propertyChangedUnder(written), access, found);
        });
    }

    void noteLooksOf(ValueFlowStatementID sharedLookup, ObjectAccess access, Vector<ObjectAccess>& found, bool followsLookupsHandedOnTo = true)
    {
        inputs.forEachMadeNodeAskedForBy(sharedLookup, kindsOfMadeNodes({ ValueFlowMadeNodeKind::Read, ValueFlowMadeNodeKind::Lookup }), false, [&](const ValueFlowNodeKey& key, ValueFlowStatementID made) {
            if (key.kind == ValueFlowMadeNodeKind::Lookup) {
                if (followsLookupsHandedOnTo && made && made != sharedLookup && isSharedLookup(made) && reachesFollowedObject(made))
                    noteLooksOf(made, access, found, false);
            } else if (namesOfValues.contains(key.name().kind))
                noteLook(key.set, key.name(), access, found);
        });
    }

    bool holdsFewMembers(const ValueFlowOperand& operand) const
    {
        ValueSetID held = operand.node ? solver.possibleValuesOf(operand.node) : ValueSets::nothing;
        return !ValueSets::isComplement(held) && sets.size(held) <= inputs.build.limit(LimitID::MaxValueFlowMembersToldApart);
    }

    // The type of a value behind a check holds only where the check passes, and the generic version stores the value all the same.
    OptionSet<WriteTrait> whatStoredValueKeeps(const ValueFlowStatement& statement, VariableID variable)
    {
        constexpr OptionSet<WriteTrait> everything { WriteTrait::KeepsDeclaredType, WriteTrait::KeepsDeclaredIntegers };
        const DeclaredType& declared = inputs.program.at(variable).declared;
        if (declared.isExpected() || inputs.program.at(declared.written()).isAnything())
            return everything;
        std::optional<uint32_t> index = indexOfValueStoredIntoProperty(statement.opcode);
        if (!index || !holdsFewMembers(inputs.bodies().operandsOf(statement)[*index]))
            return { };
        const Function& function = inputs.program.at(statement.function);
        const Type& type = inputs.program.at(*declared.established());
        ValueID stored = function.at(statement.site).operand(*index);
        const BitVector* valuesBehindChecks = inputs.bodies().valuesBehindChecks(statement.function);
        bool isBehindCheck = !valuesBehindChecks || valuesBehindChecks->get(stored.index());
        bool acceptsIntegers = type.primitives & Primitive::Integer && (isBehindCheck || inputs.program.typeOf(function, stored).integers.isSubsetOf(type.integers));
        bool isInside = true;
        bool storesIntegers = false;
        ValueFlowMembers members = forEachMemberOf(inputs, inputs.bodies().operandsOf(statement)[*index], [&](ValueFlowObjectID member) {
            if (sets.contains(valuesInsideType[variable.index()], member))
                return;
            storesIntegers |= member == ValueFlowUniverse::primitive(PrimitiveSet::Integer);
            isInside &= acceptsIntegers && member == ValueFlowUniverse::primitive(PrimitiveSet::Integer);
        });
        if (!isInside || members != ValueFlowMembers::Listed)
            return { };
        return storesIntegers && isBehindCheck ? OptionSet<WriteTrait> { WriteTrait::KeepsDeclaredType } : everything;
    }

    void noteStore(const ValueFlowStatement* statement, ValueSetID storedThrough, ValueFlowName name, ObjectAccess access, Vector<ObjectAccess>& found)
    {
        forEachObjectStoredThrough(storedThrough, [&](ValueFlowObjectID object, bool isListed) {
            if (isNamespace(object))
                return;
            access.actor = actorOf(solver.statementAt(access.by), name, isListed);
            access.form = formOf(solver.statementAt(access.by), name);
            bool namesStaticField = false;
            forEachVariableNamedBy(object, name, [&](const VariableOnObject& variable) {
                namesStaticField |= name.kind == ValueFlowNameKind::Property && !variable.mayLieUnderAnyName;
                access.variable = variable.variable;
                OptionSet<WriteTrait> traits;
                if (statement)
                    traits.add(WriteTrait::IsOperationOfBody);
                if (statement && indexOfValueStoredIntoProperty(statement->opcode))
                    traits.add(whatStoredValueKeeps(*statement, variable.variable) | WriteTrait::SpellsItsValue);
                access.traits = traits.toRaw();
                found.append(access);
            });
            if (!namesStaticField && (!statement || statement->opcode != Opcode::DefineMethod))
                setFlag(isClassObjectStoredUnderOtherName, inputs.universe().closureFunction(object).index());
        });
    }

    void noteTestBy(const ValueFlowStatement& statement, ValueSetID chain, ObjectAccess access, Vector<ObjectAccess>& found)
    {
        if (statement.opcode == Opcode::PropHas || statement.opcode == Opcode::ScopeHasBinding)
            noteLook(chain, nameOf(inputs.program.at(statement.function).at(statement.site).key()), access, found);
        if (statement.opcode != Opcode::ElemHas)
            return;
        Vector<PropertyKeyID, 4> keys;
        const ValueFlowOperand& property = inputs.bodies().operandsOf(statement)[OperandIndex::ElemHas::property];
        bool areListed = holdsFewMembers(property) && forEachMemberOf(inputs, property, [&](ValueFlowObjectID member) {
            keys.append(inputs.universe().keyOf(member));
        }) == ValueFlowMembers::Listed;
        if (!areListed || keys.contains(PropertyKeyID())) {
            noteLook(chain, everyPropertyName, access, found);
            return;
        }
        for (PropertyKeyID key : keys)
            noteLook(chain, nameOf(key), access, found);
    }

    void collectAccessesOf(ValueFlowStatementID id, Vector<ObjectAccess>& found)
    {
        const ValueFlowStatement& statement = solver.statementAt(id);
        bool isMadeByRule = solver.isMadeByRule(id);
        bool hasRun = isMadeByRule ? !solver.isAtRest(id) : !statement.function || inputs.isReached(statement.function);
        if (!hasRun || isSharedLookup(id))
            return;

        bool isOperation = !isMadeByRule && statement.kind == ValueFlowStatementKind::Operation && statement.function && statement.site;
        if (isOperation && statement.opcode == Opcode::NewClass)
            return;
        auto accessOf = [&](ObjectAccessKind kind) {
            ObjectAccess access { { }, { }, { }, kind };
            access.by = id;
            if (isOperation) {
                access.function = statement.function;
                access.site = statement.site;
            }
            return access;
        };
        bool answersWhatItReads = isOperation && (statement.opcode == Opcode::PropGet || statement.opcode == Opcode::ElemGet);

        inputs.forEachMadeNodeAskedForBy(id, kindsOfMadeNodes({ ValueFlowMadeNodeKind::Read, ValueFlowMadeNodeKind::Written, ValueFlowMadeNodeKind::Lookup }), false, [&](const ValueFlowNodeKey& key, ValueFlowStatementID made) {
            ValueFlowNameKind kind = key.name().kind;
            if (key.kind == ValueFlowMadeNodeKind::Read && namesOfValues.contains(kind))
                noteLook(key.set, key.name(), accessOf(ObjectAccessKind::Observation), found);
            if (key.kind == ValueFlowMadeNodeKind::Written && namesOfValuesAndAccessors.contains(kind))
                noteStore(isOperation ? &statement : nullptr, key.set, key.name(), accessOf(ObjectAccessKind::Store), found);
            if (key.kind == ValueFlowMadeNodeKind::Written && namesOfRemovalsAndRedefinitions.contains(kind))
                noteChangeAskedOfNamespaces(key.set, key.name(), accessOf(ObjectAccessKind::Observation), found);
            if (key.kind != ValueFlowMadeNodeKind::Lookup || !made || isLookupOfCalleeByCall(inputs, statement, key))
                return;
            if (kind == ValueFlowNameKind::PrototypeLink) {
                if (isOperation)
                    noteTestBy(statement, solver.possibleValuesOf(solver.statementAt(made).result), accessOf(ObjectAccessKind::Observation), found);
            } else if (isSharedLookup(made) && reachesFollowedObject(made))
                noteLooksOf(made, accessOf(answersWhatItReads ? ObjectAccessKind::Read : ObjectAccessKind::Observation), found);
        });
    }

    void noteTestBy(const ValueFlowBuiltinApplication& application, const BuiltinOverload& overload, const BuiltinPropertyAction& action, SetsMeetingOneSet& setsWithFollowedObjects, Vector<ObjectAccess>& found)
    {
        const Program& program = inputs.program;
        const ValueFlowUniverse& universe = inputs.universe();
        ObjectAccess access { { }, application.function, application.site, ObjectAccessKind::Observation, Actor::Table };

        ValueSetID lookedInto = application.valueAt(action.object, heap.maker());
        const BuiltinParameter* parameter = program.builtins().parameterFor(overload, action.object);
        bool looksAtOwnPropertyAlone = parameter && parameter->roles.contains(BuiltinOperandRole::KeysAreNotListed) && !parameter->protocols.contains(Protocol::PrototypeChain);
        if (!looksAtOwnPropertyAlone)
            lookedInto = heap.chainOf(lookedInto);
        if (!setsWithFollowedObjects.contains(lookedInto))
            return;

        switch (action.key.kind) {
        case BuiltinKeyKind::None:
            return;
        case BuiltinKeyKind::Names:
            for (BuiltinName name : action.key.names)
                noteLook(lookedInto, nameOf(program.keyOf(name)), access, found);
            return;
        case BuiltinKeyKind::Operand: {
            ValueSetID keys = application.valueAt(*action.key.operand, heap.maker());
            if (sets.isEmpty(keys) || !sets.isSubset(keys, universe.keys()) || sets.meet(keys, universe.builtinTables().heap().keysNobodyKnows()) || sets.size(keys) > inputs.build.limit(LimitID::MaxValueFlowMembersToldApart))
                break;
            sets.forEachMember(keys, [&](ValueFlowObjectID constant) {
                noteLook(lookedInto, nameOf(universe.keyOf(constant)), access, found);
            });
            return;
        }
        case BuiltinKeyKind::Any:
        case BuiltinKeyKind::Indexes:
        case BuiltinKeyKind::ElementsOf:
            break;
        }
        noteLook(lookedInto, everyPropertyName, access, found);
    }

    void collectTestsOfBuiltins(Vector<ObjectAccess>& found)
    {
        const BuiltinTable& builtins = inputs.program.builtins();
        BitVector builtinsThatTest = builtinsWithOverload(builtins, [](const BuiltinOverload& overload) {
            return std::ranges::any_of(overload.flow.propertyActions, testsProperty);
        });
        SetsMeetingOneSet setsWithFollowedObjects(sets, setOfObjects);
        forEachBuiltinApplication(inputs, heap, builtinsThatTest, [&](const ValueFlowBuiltinApplication& application) {
            for (const BuiltinOverload& overload : builtins.overloadsOf(application.builtin)) {
                if (overload.mode != application.mode())
                    continue;
                for (const BuiltinPropertyAction& action : overload.flow.propertyActions) {
                    if (testsProperty(action))
                        noteTestBy(application, overload, action, setsWithFollowedObjects, found);
                }
            }
        });
    }

    // The walk follows a store through the unknown object to what has escaped. It follows none through the untracked object.
    OptionSet<Actor> actorsThisWalkDoesNotFollowTo(ValueFlowObjectID object) const { return inputs.isLost(object) ? OptionSet<Actor> { Actor::ReceiversNobodyKnows } : OptionSet<Actor> { }; }

    // A namespace object stays open to everybody until somebody says here who may have got hold of it.
    void addNamespaceObjects()
    {
        const Program& program = inputs.program;
        bool modulesAreNamedAtRunTime = unseenCodeCanRun(inputs.solution) || modulesAreLoadedByName(inputs.solution);
        bool filesOfProgramAreNamedAtRunTime = filesOfProgramAreLoadedByName(inputs.solution);
        ValueSetID created = solver.possibleValuesOf(inputs.bodies().nodes().ofHost(ValueFlowHostNode::CreatedNamespaces));
        if (sets.meet(created, inputs.universe().placeholdersAlone()))
            return;
        for (uint32_t index = 0; index < program.modules.size(); ++index) {
            const Module& module = program.modules[index];
            if (module.kind == ModuleKind::Runtime || module.kind == ModuleKind::External || !inputs.build.isEnabled(FlowResultKind::ObjectUses, index))
                continue;
            // A file that the host runs gets the object of a file with code too, and the run time lets it read only what is not closed: listExportsClosedToOutside().
            bool showsOnlyWhatIsOpen = solver.input().refusedCode.hostRunsFilesOutsideImage && showsOutsideOnlyWhatIsOpen(module);
            if ((answersOnlyToLoadsOfProgram(program, ModuleID(index)) ? filesOfProgramAreNamedAtRunTime : modulesAreNamedAtRunTime) && mayAnswerToName(program, ModuleID(index)) && !showsOnlyWhatIsOpen)
                continue;
            LedgerOfVariables::NamespaceObject& entry = locations.namespaceOf(ModuleID(index));
            ValueFlowObjectID namespaceObject = inputs.universe().namespaceOf(ModuleID(index));
            entry.isCreated = sets.contains(created, namespaceObject);
            if (!entry.isCreated) {
                entry.open = { };
                continue;
            }
            entry.open = actorsOpenOn(inputs, namespaceObject) | actorsThisWalkDoesNotFollowTo(namespaceObject);
            // What is asked under an index names no export.
            bool hasExportUnderIndex = std::ranges::any_of(module.exports, [&](const ModuleExport& entry) {
                return mayBeElementKey(program.strings.nameClass(entry.name));
            });
            // A namespace object is frozen or sealed export by export.
            constexpr OptionSet<ObjectMutation> marksOfBuiltinsThatAskEveryExport { ObjectMutation::ReachNativeCode, ObjectMutation::ChangeIntegrity };
            if (inputs.hasEscaped(namespaceObject))
                entry.readerUnderNamesNobodyLists = Actor::ReceiversNobodyKnows;
            else if (markedMutationsOf(inputs, namespaceObject).containsAny(marksOfBuiltinsThatAskEveryExport))
                entry.readerUnderNamesNobodyLists = Actor::Table;
            else if (hasExportUnderIndex)
                entry.readerUnderNamesNobodyLists = Actor::KeysNobodyKnows;
            else if (commonJsModuleCapturedBy(program, module))
                entry.readerUnderNamesNobodyLists = Actor::Loader;
            if (!entry.open.isEmpty() || entry.readerUnderNamesNobodyLists)
                continue;
            objects.append(namespaceObject);
            for (uint32_t position = 0; position < module.exports.size(); ++position) {
                const ModuleExport& entry = module.exports[position];
                if (entry.kind != ModuleExportKind::Variable || !entry.variable)
                    continue;
                PropertyKeyID key = module.namespaceLayout ? program.at(module.namespaceLayout).fields[position].key : program.types.find({ .kind = PropertyKeyKind::Text, .text = entry.name }).value_or(PropertyKeyID());
                variables.append({ namespaceObject, entry.variable, key, false });
            }
        }
    }

    // An actor that is open on the class object is open on each of its static fields.
    void addClassObjects(Vector<ValueFlowObjectID>& escapedObjects)
    {
        for (const Layout& layout : inputs.program.layouts) {
            if (layout.staticFields.isEmpty())
                continue;
            ValueFlowObjectID classObject = layout.constructor ? inputs.universe().closureOf(layout.constructor) : ValueFlowObjectID();
            OptionSet<Actor> open = classObject ? actorsOpenOn(inputs, classObject) | actorsThisWalkDoesNotFollowTo(classObject) : everyActor;
            OptionSet<ObjectMutation> marked = classObject ? markedMutationsOf(inputs, classObject) : OptionSet<ObjectMutation> { };
            bool isFollowed = open.isEmpty();
            bool isInHandsNobodyKnows = isFollowed && inputs.hasEscaped(classObject);
            bool isHandedToBuiltins = isFollowed && marked.contains(ObjectMutation::ReachNativeCode);
            if (isFollowed)
                objects.append(classObject);
            if (isInHandsNobodyKnows || isHandedToBuiltins)
                escapedObjects.append(classObject);
            for (const StaticField& field : layout.staticFields) {
                if (!isFollowed) {
                    locations.leaveOpen(locations.of(field.variable), open);
                    continue;
                }
                variables.append({ classObject, field.variable, field.key, !field.key || inputs.program.isIndexLike(field.key) });
                auto note = [&](ObjectAccessKind kind, Actor actor, WriteForm form) {
                    accessesSaidByMarks.append({ field.variable, { }, { }, kind, actor, form });
                };
                if (isInHandsNobodyKnows)
                    note(ObjectAccessKind::Observation, Actor::ReceiversNobodyKnows, WriteForm::Store);
                if (isHandedToBuiltins)
                    note(ObjectAccessKind::Observation, Actor::Table, WriteForm::Store);
                if (marked.contains(ObjectMutation::DeleteField))
                    note(ObjectAccessKind::Store, Actor::KeysNobodyKnows, WriteForm::Delete);
                if (marked.contains(ObjectMutation::RedefineProperty))
                    note(ObjectAccessKind::Store, Actor::KeysNobodyKnows, WriteForm::Define);
                if (marked.contains(ObjectMutation::ChangeIntegrity))
                    note(ObjectAccessKind::Store, Actor::KeysNobodyKnows, WriteForm::Freeze);
            }
        }
    }

    void collect()
    {
        const Program& program = inputs.program;
        Vector<ValueFlowObjectID> escapedObjects;
        addNamespaceObjects();
        addClassObjects(escapedObjects);
        locations.didHear(Report::HoldersOfVariables);
        if (objects.isEmpty())
            return;

        setOfObjects = heap.maker().of(objects.span());
        setOfEscapedObjects = heap.maker().of(escapedObjects.span());
        AOT::stableSortInLine(variables, { }, &VariableOnObject::object);
        indexVariables();
        setsWithObject.fill(0, sets.endOfTableBySet() / 4);
        lookupReachesFollowedObject.fill(0, solver.numberOfStatements() - solver.input().statements.size());
        isNamespaceReadUnderUnknownName.fill(0, program.modules.size());
        isClassObjectStoredUnderOtherName.fill(0, program.functions.size());
        valuesInsideType.fill(ValueSets::nothing, program.variables.size());
        for (const VariableOnObject& variable : variables) {
            const DeclaredType& declared = program.at(variable.variable).declared;
            if (!isNamespace(variable.object) && !declared.isExpected())
                valuesInsideType[variable.variable.index()] = inputs.universe().membersSurelyInside(heap.maker(), program, *declared.established());
        }
        findLookupsReachingFollowedObjects();

        Vector<ObjectAccess> testsOfBuiltins;
        runTasksInParallel(inputs.loops(), {
            [&] {
                accesses = collectInParallel<ObjectAccess>(inputs.loops(), solver.numberOfStatements(), [&](unsigned index, Vector<ObjectAccess>& found) {
                    collectAccessesOf(ValueFlowStatementID(index), found);
                });
            },
            [&] {
                collectTestsOfBuiltins(testsOfBuiltins);
            },
        });
        accesses.appendVector(WTF::move(testsOfBuiltins));
    }

    const ValueFlowResultInputs& inputs;
    LedgerOfVariables& locations;
    const ValueFlowSolver& solver;
    const ValueSets& sets;
    ValueFlowHeapView heap;
    Vector<ObjectAccess> accesses;
    Vector<ObjectAccess> accessesSaidByMarks;
    Vector<ValueFlowObjectID> objects;
    ValueSetID setOfObjects { ValueSets::nothing };
    ValueSetID setOfEscapedObjects { ValueSets::nothing };
    Vector<VariableOnObject> variables;
    Vector<uint32_t> positionsByKey;
    Vector<uint32_t> positionsUnderAnyName;
    Vector<uint8_t> setsWithObject;
    Vector<uint8_t> lookupReachesFollowedObject;
    Vector<uint8_t> isNamespaceReadUnderUnknownName;
    Vector<uint8_t> isClassObjectStoredUnderOtherName;
    Vector<ValueSetID> valuesInsideType;
};

void AccessesThroughHoldersDeleter::operator()(AccessesThroughHolders* accesses) const
{
    delete accesses;
}

void collectAccessesThroughHolders(const ValueFlowResultInputs& inputs, LedgerOfVariables& locations)
{
    locations.accessesThroughHolders.reset(new AccessesThroughHolders(inputs, locations));
    locations.accessesThroughHolders->collect();
}

void reportAccessesThroughHolders(const ValueFlowResultInputs& inputs, const ValueFlowUsesOfHeap& usesOfHeap, LedgerOfVariables& locations)
{
    const Program& program = inputs.program;
    AccessesThroughHolders& collected = *locations.accessesThroughHolders;
    Vector<ObjectAccess>& accesses = collected.accesses;
    if (!collected.objects.isEmpty()) {
        collected.forEachObjectIn(objectsThatMayBeEnumerated(usesOfHeap), [&](ValueFlowObjectID object) {
            collected.noteLook(collected.heap.maker().of(object), everyPropertyName, { }, accesses);
        });
        for (ValueFlowObjectID object : collected.objects) {
            if (collected.isNamespace(object) && collected.isNamespaceReadUnderUnknownName[inputs.universe().moduleOf(object).index()])
                locations.namespaceOf(inputs.universe().moduleOf(object)).readerUnderNamesNobodyLists = Actor::KeysNobodyKnows;
        }
        BitVector classObjectsStoredUnderStaticFieldsAlone(program.layouts.size());
        for (uint32_t index = 0; index < program.layouts.size(); ++index) {
            const Layout& layout = program.layouts[index];
            if (!layout.staticFields.isEmpty() && layout.constructor && inputs.sets().contains(collected.setOfObjects, inputs.universe().closureOf(layout.constructor)) && !collected.isClassObjectStoredUnderOtherName[layout.constructor.index()])
                classObjectsStoredUnderStaticFieldsAlone.quickSet(index);
        }
        locations.setClassObjectsStoredUnderStaticFieldsAlone(WTF::move(classObjectsStoredUnderStaticFieldsAlone));
    }
    accesses.appendVector(WTF::move(collected.accessesSaidByMarks));
    sortAndRemoveDuplicates(inputs.loops(), accesses);

    Vector<WriteAt> writes;
    Vector<ReadAt> reads;
    Vector<ObjectAccess> readsThatAnswer;
    for (const ObjectAccess& access : accesses) {
        uint32_t location = locations.of(access.variable);
        switch (access.kind) {
        case ObjectAccessKind::Read:
            readsThatAnswer.append(access);
            break;
        case ObjectAccessKind::Observation:
            reads.append({ location, { access.actor, ReadForm::AsValue, access.by, access.function, access.site, false } });
            break;
        case ObjectAccessKind::Store:
            writes.append({ location, { access.actor, access.form, access.by, { }, access.function, access.site, OptionSet<WriteTrait>::fromRaw(access.traits) } });
            break;
        }
    }
    AOT::stableSort(readsThatAnswer, { }, &ObjectAccess::function);
    for (size_t first = 0, end = 0; first < readsThatAnswer.size(); first = end) {
        FunctionID id = readsThatAnswer[first].function;
        while (end < readsThatAnswer.size() && readsThatAnswer[end].function == id)
            ++end;
        forEachEventOfReads(program.at(id), readsThatAnswer.span().subspan(first, end - first), [](const ObjectAccess& read) {
            return read.site;
        }, [&](const ObjectAccess& read, ReadForm form) {
            reads.append({ locations.of(read.variable), { read.actor, form, read.by, read.function, read.site, false } });
        });
    }
    locations.addThroughHolders(WTF::move(writes), WTF::move(reads));
    locations.accessesThroughHolders = nullptr;
    locations.didHear(Report::AccessesThroughHolders);
}

// A read that names the variable finds it without the namespace object.
static void listExportsReadByName(const ValueFlowResultInputs& inputs, const Locations& locations, std::span<ModuleFlow> modules)
{
    const Program& program = inputs.program;
    for (uint32_t index = 0; index < program.modules.size(); ++index) {
        if (modules[index].namespaceUse == NamespaceUse::Unknown)
            continue;
        Vector<VariableID> listed;
        for (const ModuleExport& entry : program.modules[index].exports) {
            if (entry.kind != ModuleExportKind::Variable || !entry.variable)
                continue;
            if (!isReadOnlyByOperationsThatNameIt(locations.readersAt(locations.of(entry.variable))))
                listed.append(entry.variable);
        }
        AOT::sort(listed);
        removeRepeatedElements(listed);
        modules[index].exportsReadByName = WTF::move(listed);
    }
}

// Where the host runs files that the build has not seen. Such a file may ask an entry point for any name. What flow has concluded of an object or a function in known hands holds only while that file does not get it.
static void listExportsClosedToOutside(const ValueFlowResultInputs& inputs, std::span<ModuleFlow> modules)
{
    if (!inputs.solver().input().refusedCode.hostRunsFilesOutsideImage)
        return;
    const Program& program = inputs.program;
    BorrowedValueSetMaker borrowedMaker(inputs.makerPool());
    ValueSetID objectsInKnownHands = borrowedMaker.get().intersect(inputs.universe().knownObjects(), ValueSets::complement(inputs.solver().possibleValuesOf(ValueFlowProgramNodes::escaped())));
    for (uint32_t index = 0; index < program.modules.size(); ++index) {
        if (modules[index].namespaceUse == NamespaceUse::Unknown)
            continue;
        Vector<VariableID> listed;
        for (const ModuleExport& entry : program.modules[index].exports) {
            if (entry.kind != ModuleExportKind::Variable || !entry.variable || program.at(entry.variable).flags.contains(VariableFlags::IsReplaceable))
                continue;
            ValueSetID held = inputs.solver().possibleValuesOf(inputs.bodies().nodes().variable(entry.variable));
            if (!ValueSets::isComplement(held) && inputs.sets().meet(held, objectsInKnownHands))
                listed.append(entry.variable);
        }
        AOT::sort(listed);
        removeRepeatedElements(listed);
        modules[index].exportsClosedToOutside = WTF::move(listed);
    }
}

void computeLocationResults(const ValueFlowResultInputs& inputs, Ledger& ledger, LedgerOfVariables& locations, FlowResultTables& tables)
{
    const Program& program = inputs.program;
    FlowApplication& application = *tables.application;

    reportScopesOfEvaluatedText(program, locations);
    for (uint32_t index = 0; index < program.modules.size(); ++index) {
        tables.wholeProgram.modules[index].namespaceUse = namespaceUseOf(locations.reachersOfNamespace(ModuleID(index)), locations.readersOfNamespaceUnderNamesNobodyLists(ModuleID(index))).state;
        if (locations.isNamespaceNamedByOperation(ModuleID(index)))
            tables.wholeProgram.modulesWithNamespaceNamedByOperations.set(index);
    }
    reportModules(inputs, tables.wholeProgram.modules.span(), locations);
    locations.close();
    ledger.install(LedgerPartKind::Variables, locations);
    listExportsReadByName(inputs, locations, tables.wholeProgram.modules.mutableSpan());
    listExportsClosedToOutside(inputs, tables.wholeProgram.modules.mutableSpan());

    auto flowOf = [&](const Place& place, uint32_t switchSubject) {
        LocationFlow flow;
        if (!inputs.build.isEnabled(FlowResultKind::StoreCounts, switchSubject))
            flow.cause = UnprovenCause::ResultDisabled;
        else
            claimWritesOf(flow, ledger.writersOf(place));
        if (inputs.build.isEnabled(FlowResultKind::ObjectUses, switchSubject))
            claimReadsOf(flow, ledger.readersOf(place));
        return flow;
    };

    forEachIndexInParallel(inputs.loops(), program.variables.size(), [&](unsigned index) {
        application.variables[index].flow = flowOf(Place::variable(VariableID(index)), index);
    });
    forEachIndexInParallel(inputs.loops(), program.layouts.size(), [&](unsigned index) {
        if (!program.layouts[index].isEnvironment())
            return;
        Vector<FlowFieldUpdate>& fields = application.layouts[index].fields;
        for (uint32_t field = 0; field < fields.size(); ++field)
            fields[field].flow.location = flowOf(Place::field(program, LayoutID(index), field), index);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
