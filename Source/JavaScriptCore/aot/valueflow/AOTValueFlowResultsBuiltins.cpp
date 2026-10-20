/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowResultsInternal.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTFlowBuiltinLocations.h"
#include "AOTFlowUnseenCode.h"
#include "AOTValueFlowResultsUnseenCode.h"
#include <algorithm>
#include <tuple>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OptionSet<BuiltinUse> callingBuiltinUses { BuiltinUse::Called, BuiltinUse::Constructed, BuiltinUse::Escaped };
static constexpr OptionSet<BuiltinUse> accessingBuiltinUses { BuiltinUse::Read, BuiltinUse::Written };

static Vector<BuiltinUseRecord> collectBuiltinUses(const ValueFlowResultInputs& inputs)
{
    Vector<BuiltinUseRecord> uses;
    for (uint32_t index = 0, count = inputs.program.builtins().builtinCount(); index < count; ++index) {
        OptionSet<BuiltinUse> usesOfBuiltin = inputs.solver().usesOf(BuiltinID(index));
        if (!usesOfBuiltin.isEmpty())
            uses.append(BuiltinUseRecord { index, static_cast<BuiltinUse>(usesOfBuiltin.toRaw()), noWitness });
    }
    return uses;
}

// Native code of the feature creates the value of a data property on the first read, and what the property holds is called as an unknown function.
// So is a function that a lookup through the unknown object has found: the lookup is a read of it, and no call lists it.
static OptionSet<BuiltinUse> usesEnteringCode(BuiltinKind kind)
{
    switch (kind) {
    case BuiltinKind::Accessor:
    case BuiltinKind::DataProperty:
        return callingBuiltinUses | accessingBuiltinUses;
    case BuiltinKind::Constructor:
    case BuiltinKind::Function:
    case BuiltinKind::Method:
        return callingBuiltinUses | BuiltinUse::Read;
    case BuiltinKind::Namespace:
    case BuiltinKind::Prototype:
    case BuiltinKind::Constant:
    case BuiltinKind::NativeBinding:
        return callingBuiltinUses;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static BitVector findNamespacesOfModules(const BuiltinTable& builtins)
{
    BitVector namespaces(builtins.builtinCount());
    for (uint32_t index = 0; index < builtins.moduleCount(); ++index) {
        const RuntimeModuleData& module = builtins.at(RuntimeModuleID(index));
        if (module.namespaceObject)
            namespaces.quickSet(module.namespaceObject.index());
    }
    return namespaces;
}

// A literal, a coroutine and a rest parameter make an instance of a class without naming the constructor.
static void addConstructorsOfAllocatedClasses(const ValueFlowResultInputs& inputs, BitVector& builtinsWhoseCodeRuns)
{
    const BuiltinTable& builtins = inputs.program.builtins();
    const ValueFlowUniverse& universe = inputs.universe();
    BitVector allocatedClasses(builtins.classCount());
    for (uint32_t index = 0; index < universe.numberOfObjects(); ++index) {
        ValueFlowObjectID object(index);
        switch (universe.kindOf(object)) {
        case ValueFlowObjectKind::CoroutineResult:
        case ValueFlowObjectKind::RestArray:
        case ValueFlowObjectKind::Site:
            break;
        case ValueFlowObjectKind::Primitive:
        case ValueFlowObjectKind::UnknownObject:
        case ValueFlowObjectKind::Builtin:
        case ValueFlowObjectKind::ModuleBuiltin:
        case ValueFlowObjectKind::Namespace:
        case ValueFlowObjectKind::Closure:
        case ValueFlowObjectKind::Instance:
        case ValueFlowObjectKind::Prototype:
        case ValueFlowObjectKind::ResultOfBuiltin:
        case ValueFlowObjectKind::Key:
            continue;
        }
        BuiltinClassID builtinClass = universe.classOf(object);
        if (builtinClass && inputs.isReached(universe.functionOf(object)))
            allocatedClasses.quickSet(builtinClass.index());
    }
    for (size_t index : allocatedClasses) {
        if (BuiltinID constructor = builtins.at(BuiltinClassID(index)).constructor)
            builtinsWhoseCodeRuns.quickSet(builtins.canonicalID(constructor).index());
    }
}

// The table attaches features to the entry of the object, and an access is recorded on the location that was accessed.
static BitVector findBuiltinsWhoseCodeRuns(const ValueFlowResultInputs& inputs)
{
    const BuiltinTable& builtins = inputs.program.builtins();
    BitVector namespacesOfModules = findNamespacesOfModules(builtins);
    BitVector builtinsWhoseCodeRuns(builtins.builtinCount());
    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        BuiltinID builtin(index);
        OptionSet<BuiltinUse> enteringUses = usesEnteringCode(builtins.at(builtin).kind);
        if (namespacesOfModules.quickGet(index))
            enteringUses.add(accessingBuiltinUses);
        if (inputs.solver().usesOf(builtin).containsAny(enteringUses))
            builtinsWhoseCodeRuns.quickSet(builtins.canonicalID(builtin).index());
    }
    addConstructorsOfAllocatedClasses(inputs, builtinsWhoseCodeRuns);
    return builtinsWhoseCodeRuns;
}

// No condition of a line is refuted: nobody has looked at the operands.
static Vector<FeatureUseRecord> collectFeatureUses(const ValueFlowResultInputs& inputs, const BitVector& builtinsWhoseCodeRuns)
{
    auto orderOf = [](const FeatureUseRecord& use) {
        return std::tuple(use.feature, use.builtin);
    };
    Vector<FeatureUseRecord> uses;
    for (size_t index : builtinsWhoseCodeRuns) {
        for (const BuiltinFeatureUse& line : inputs.program.builtins().featureUsesOf(BuiltinID(index)))
            uses.append(FeatureUseRecord { static_cast<uint32_t>(line.feature), static_cast<uint32_t>(index), noWitness });
    }
    std::ranges::sort(uses, { }, orderOf);
    uses.shrink(std::ranges::unique(uses, { }, orderOf).begin() - uses.begin());
    return uses;
}

static bool takesLocale(const BuiltinTable& builtins, BuiltinID builtin)
{
    return std::ranges::any_of(builtins.overloadsOf(builtin), [](const BuiltinOverload& overload) {
        return overload.isUndeclared() || std::ranges::any_of(overload.parameters, [](const BuiltinParameter& parameter) {
            return parameter.roles.contains(BuiltinOperandRole::Locale);
        });
    });
}

// Which locale a call names is in the text of an operand, which nobody has looked at.
static Vector<FlowLocaleUse> collectLocaleUses(const ValueFlowResultInputs& inputs, const BitVector& builtinsWhoseCodeRuns, std::span<const UnseenCodeRecord> unseenCode)
{
    bool namesAnyLocale = std::ranges::any_of(unseenCode, admitsUnknownJavaScript, &UnseenCodeRecord::cause);
    BuiltinID builtinTakingLocale;
    for (size_t index : builtinsWhoseCodeRuns) {
        if (!builtinTakingLocale && takesLocale(inputs.program.builtins(), BuiltinID(index)))
            builtinTakingLocale = BuiltinID(index);
    }
    if (!namesAnyLocale && !builtinTakingLocale)
        return { };
    return { FlowLocaleUse { LocaleUseKind::Unknown, { }, builtinTakingLocale, noWitness } };
}

static bool isRaised(const ValueFlowResultInputs& inputs, ValueFlowHostNode node)
{
    return !inputs.sets().isEmpty(inputs.solver().valueOf(inputs.bodies().nodes().ofHost(node)));
}

// The image admits at run time what its records name and refuses the rest, so they name what the solve has let in, no more and no less.
static Vector<UnseenCodeRecord> collectUnseenCode(const ValueFlowResultInputs& inputs)
{
    bool unseenCodeRuns = unseenCodeCanRun(inputs.solution);
    bool modulesAreNamedAtRunTime = modulesAreLoadedByName(inputs.solution);

    Vector<UnseenCodeRecord> records = unseenCodeRecordsOf(inputs.solution);
    records.removeAllMatching([&](const UnseenCodeRecord& record) {
        return admitsUnknownJavaScript(record.cause) ? !unseenCodeRuns : !modulesAreNamedAtRunTime;
    });
    if (unseenCodeRuns && !std::ranges::any_of(records, admitsUnknownJavaScript, &UnseenCodeRecord::cause))
        records.append(UnseenCodeRecord { .cause = UnseenCodeCause::RunsUnseenCode });
    if (modulesAreNamedAtRunTime && !std::ranges::any_of(records, loadsModulesByName, &UnseenCodeRecord::cause))
        records.append(UnseenCodeRecord { .cause = UnseenCodeCause::LoadsModuleByName });
    // The record that names the module cache says that the answers of the loader are replaced.
    if (isRaised(inputs, ValueFlowHostNode::AnswersOfReplacedLoader))
        records.append(UnseenCodeRecord { .cause = UnseenCodeCause::LoadsModuleByName, .builtin = Builtins::ModuleCache.index() });
    if (!inputs.build.isEnabledForProgram(FlowResultKind::ReachedFunctions))
        records.append(UnseenCodeRecord { .cause = UnseenCodeCause::FlowResultDisabled });

    auto orderOf = [](const UnseenCodeRecord& record) {
        return std::tuple(record.cause, record.builtin, record.witness.function, record.witness.value);
    };
    std::ranges::sort(records, { }, orderOf);
    records.shrink(std::ranges::unique(records, { }, orderOf).begin() - records.begin());
    return records;
}

// Every store, definition and deletion that may reach the holder under the name of a member is a use of the member, also one through the unknown object or under a key nobody knows.
static BuiltinLocationState stateOfLocation(const ValueFlowResultInputs& inputs, BuiltinID member, bool unseenCodeRuns)
{
    const BuiltinTable& builtins = inputs.program.builtins();
    if (isLockedLocation(builtins, member))
        return BuiltinLocationState::Constant;
    switch (builtins.at(member).mutability) {
    case BuiltinMutability::AlwaysLocked:
        return BuiltinLocationState::Constant;
    case BuiltinMutability::ChangesOnItsOwn:
        return BuiltinLocationState::Unknown;
    case BuiltinMutability::ConstantUnlessWritten:
        break;
    }

    OptionSet<BuiltinUse> uses = inputs.solver().usesOf(member);
    if (uses.contains(BuiltinUse::Written) || !inputs.build.isEnabled(FlowResultKind::ConstantBuiltinLocations, member.index()))
        return BuiltinLocationState::Unknown;
    if (!unseenCodeRuns)
        return BuiltinLocationState::Constant;
    // Code that the build has not seen writes what it likes. Start-up makes a location immutable that is listed as constant, if it finds the holder.
    // That does not help a member of the global object: a script declares a `let` of the name, which every free name finds first, and writes nothing.
    bool isLockedAtStartUp = !uses.isEmpty() && canLockAtStartUp(builtins, member) && builtins.at(member).holder != Builtins::GlobalThis;
    return isLockedAtStartUp ? BuiltinLocationState::Constant : BuiltinLocationState::Unknown;
}

static void computeBuiltinLocations(const ValueFlowResultInputs& inputs, FlowBuiltinTables& tables)
{
    const BuiltinTable& builtins = inputs.program.builtins();
    bool unseenCodeRuns = unseenCodeCanRun(inputs.solution);
    auto orderOf = [](const BuiltinLocationFlow& location) {
        return std::tuple(location.holder, location.key);
    };

    tables.constantMembers = BitVector(builtins.builtinCount());
    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        BuiltinID member(index);
        const Builtin& entry = builtins.at(member);
        if (!entry.holder || !entry.name)
            continue;
        BuiltinLocationState state = stateOfLocation(inputs, member, unseenCodeRuns);
        if (state == BuiltinLocationState::Constant)
            tables.constantMembers.quickSet(index);
        if (!inputs.solver().usesOf(member).isEmpty())
            tables.locations.append(BuiltinLocationFlow { entry.holder, inputs.program.keyOf(entry.name), state, noWitness });
    }

    // Two entries may describe one location. It is constant if both say so.
    std::ranges::stable_sort(tables.locations, { }, orderOf);
    size_t size = 0;
    for (const BuiltinLocationFlow& location : tables.locations) {
        if (!size || orderOf(tables.locations[size - 1]) != orderOf(location)) {
            tables.locations[size++] = location;
            continue;
        }
        if (location.state == BuiltinLocationState::Unknown)
            tables.locations[size - 1].state = BuiltinLocationState::Unknown;
    }
    tables.locations.shrink(size);
}

// A worker runs the program once more, with variables of its own.
static ProgramInstances countProgramInstances(const ValueFlowResultInputs& inputs)
{
    const BuiltinTable& builtins = inputs.program.builtins();
    if (!inputs.build.isEnabledForProgram(FlowResultKind::ReachedFunctions) || unseenCodeCanRun(inputs.solution))
        return ProgramInstances::Unknown;
    // Two names of one built-in share the uses of neither.
    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        if (!inputs.solver().usesOf(BuiltinID(index)).containsAny(usesEnteringCode(builtins.at(BuiltinID(index)).kind)))
            continue;
        if (builtins.at(BuiltinID(index)).flags.contains(BuiltinFlag::StartsInstance) || builtins.at(builtins.canonicalID(BuiltinID(index))).flags.contains(BuiltinFlag::StartsInstance))
            return ProgramInstances::Unknown;
    }
    return ProgramInstances::AtMostOne;
}

// RegExp.$1 and its like change with every match. A read under a key nobody knows is a use of every member of the holder.
static LegacyRegExpStatics findReadsOfLegacyRegExpStatics(const ValueFlowResultInputs& inputs)
{
    const BuiltinTable& builtins = inputs.program.builtins();
    BuiltinID constructor = builtins.canonicalID(builtins.at(BuiltinClasses::RegExp).constructor);
    if (!inputs.build.isEnabled(FlowResultKind::ObjectUses, constructor.index()) || unseenCodeCanRun(inputs.solution) || inputs.hasEscaped(inputs.universe().objectOf(constructor)))
        return LegacyRegExpStatics::Unknown;

    bool isMentioned = std::ranges::any_of(builtins.membersOf(constructor), [&](BuiltinID member) {
        if (builtins.at(member).mutability != BuiltinMutability::ChangesOnItsOwn)
            return false;
        return !inputs.solver().usesOf(member).isEmpty() || !inputs.solver().usesOf(builtins.canonicalID(member)).isEmpty();
    });
    return isMentioned ? LegacyRegExpStatics::Unknown : LegacyRegExpStatics::NeverRead;
}

void computeBuiltinResults(const ValueFlowResultInputs& inputs, FlowResultTables& tables)
{
    computeBuiltinLocations(inputs, tables.builtins);
    BitVector builtinsWhoseCodeRuns = findBuiltinsWhoseCodeRuns(inputs);
    tables.wholeProgram.unseenCode = collectUnseenCode(inputs);
    tables.wholeProgram.instances = countProgramInstances(inputs);
    tables.builtins.uses = collectBuiltinUses(inputs);
    tables.builtins.legacyRegExpStatics = findReadsOfLegacyRegExpStatics(inputs);
    tables.builtins.featureUses = collectFeatureUses(inputs, builtinsWhoseCodeRuns);
    tables.builtins.localeUses = collectLocaleUses(inputs, builtinsWhoseCodeRuns, tables.wholeProgram.unseenCode.span());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
