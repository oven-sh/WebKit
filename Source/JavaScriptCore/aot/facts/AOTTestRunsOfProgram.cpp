/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTestRuns.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinIDs.h"
#include "AOTBuiltinMembers.h"
#include "AOTBuiltinTable.h"
#include "AOTFacts.h"
#include "AOTIRProgram.h"
#include "AOTNameClass.h"
#include "AOTSort.h"
#include <algorithm>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

static bool namesModuleInTestRunsOfProgram(const Program& program, ModuleID id)
{
    return id && id.index() < program.modules.size();
}

static bool markModulesThatMayBeMockedInTestRunsOfProgram(const Program& program, BitVector& mocked)
{
    size_t numberOfModules = program.modules.size();
    bool isAnyModuleMocked = false;
    for (size_t index = 0; index < numberOfModules; ++index) {
        if (!program.modules[index].flags.contains(ModuleFlags::MayBeMocked))
            continue;
        if (!std::exchange(isAnyModuleMocked, true))
            mocked.ensureSize(numberOfModules);
        mocked.quickSet(index);
    }
    if (!isAnyModuleMocked)
        return false;

    for (size_t index = 0; index < numberOfModules; ++index) {
        const Module& module = program.modules[index];
        if (mocked.quickGet(index) || module.requests.isEmpty() || !namesModuleInTestRunsOfProgram(program, module.requests[0].target))
            continue;
        if (mocked.quickGet(module.requests[0].target.index()) && isCommonJSFacade(program, module))
            mocked.quickSet(index);
    }
    return true;
}

static BitVector namespacesHandedOnInTestRunsOfProgram(const Program& program, const BitVector& mocked)
{
    BitVector handedOn(program.modules.size());
    for (size_t index : mocked) {
        for (const ModuleExport& entry : program.modules[index].exports) {
            if (entry.kind == ModuleExportKind::Namespace && namesModuleInTestRunsOfProgram(program, entry.module))
                handedOn.quickSet(entry.module.index());
        }
    }
    return handedOn;
}

static BitVector findModulesReachingMocks(const Program& program, const BitVector& mocked)
{
    size_t numberOfModules = program.modules.size();
    Vector<uint32_t> firstImporter;
    firstImporter.fill(0, numberOfModules + 1);
    for (const Module& module : program.modules) {
        for (const ModuleRequest& request : module.requests) {
            if (namesModuleInTestRunsOfProgram(program, request.target))
                ++firstImporter[request.target.index() + 1];
        }
    }
    for (size_t index = 0; index < numberOfModules; ++index)
        firstImporter[index + 1] += firstImporter[index];

    Vector<uint32_t> importers;
    importers.grow(firstImporter[numberOfModules]);
    Vector<uint32_t> nextPlace(firstImporter.span());
    for (size_t index = 0; index < numberOfModules; ++index) {
        for (const ModuleRequest& request : program.modules[index].requests) {
            if (namesModuleInTestRunsOfProgram(program, request.target))
                importers[nextPlace[request.target.index()]++] = static_cast<uint32_t>(index);
        }
    }

    BitVector leads(numberOfModules);
    Vector<uint32_t> notLookedAt;
    for (size_t index : mocked) {
        leads.quickSet(index);
        notLookedAt.append(static_cast<uint32_t>(index));
    }
    while (!notLookedAt.isEmpty()) {
        uint32_t target = notLookedAt.takeLast();
        for (uint32_t place = firstImporter[target]; place < firstImporter[target + 1]; ++place) {
            if (!leads.quickSet(importers[place]))
                notLookedAt.append(importers[place]);
        }
    }
    return leads;
}

MocksOfProgram findMocksOfProgram(const Program& program)
{
    MocksOfProgram mocks;
    if (!markModulesThatMayBeMockedInTestRunsOfProgram(program, mocks.modulesThatMayBeMocked))
        return mocks;
    mocks.isAnyModuleMocked = true;
    mocks.namespacesHandedOnByMockedModules = namespacesHandedOnInTestRunsOfProgram(program, mocks.modulesThatMayBeMocked);
    mocks.modulesReachingMocks = findModulesReachingMocks(program, mocks.modulesThatMayBeMocked);
    return mocks;
}

bool mayComeThroughMock(const Program& program, const MocksOfProgram& mocks, ModuleID exporter, uint32_t indexOfExport)
{
    if (!mocks.isAnyModuleMocked)
        return false;
    if (!exporter || mocks.modulesThatMayBeMocked.get(exporter.index()))
        return true;
    const auto& exports = program.at(exporter).exports;
    if (indexOfExport >= exports.size())
        return mocks.modulesReachingMocks.get(exporter.index());

    const ModuleExport& entry = exports[indexOfExport];
    switch (entry.kind) {
    case ModuleExportKind::Variable:
        if (!entry.variable || entry.variable.index() >= program.variables.size())
            return true;
        if (program.at(entry.variable).module != exporter && mocks.modulesReachingMocks.get(exporter.index()))
            return true;
        return false;
    case ModuleExportKind::Namespace:
        if (!entry.module)
            return true;
        if (mocks.modulesThatMayBeMocked.get(entry.module.index()))
            return true;
        if (mocks.namespacesHandedOnByMockedModules.get(entry.module.index()))
            return true;
        return false;
    case ModuleExportKind::Indirect:
        return mocks.isAnyModuleMocked;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool holderTakesNameInTestRun(const Program& program, BuiltinID canonicalHolder, PropertyKeyID key)
{
    const PropertyKey& record = program.at(key);
    if (record.kind != PropertyKeyKind::Text || mayBeElementKey(program.nameClassOf(key)))
        return false;
    if (findOwnMember(program, canonicalHolder, key))
        return false;
    return program.builtins().canTakeNameInTestRuns(canonicalHolder, program.builtinName(record.text));
}

static void sortNamesInTestRunsOfProgram(Vector<NameAddedByTests>& names)
{
    AOT::sort(names);
    removeRepeatedElements(names);
}

void openNamesThatTestsAdd(Program& program, Vector<NameAddedByTests>&& ofLinked, Vector<NameAddedByTests>&& ofHeld)
{
    Vector<NameAddedByTests> names = WTF::move(ofLinked);
    names.appendVector(ofHeld);
    sortNamesInTestRunsOfProgram(names);
    for (const NameAddedByTests& added : names) {
        if (!program.isOpenedSomewhere(added.name) || !program.isOpened(added.holder, added.name))
            program.openNameForTestRun(added.holder, added.name);
    }
}

static BitVector lostBuiltinsInTestRunsOfProgram(const Program& program, const LookupsPastProgramObjects& lookups)
{
    const BuiltinTable& builtins = program.builtins();
    BitVector lost(builtins.builtinCount());
    Vector<BuiltinID> notLookedAt;
    auto add = [&](BuiltinID object) {
        if (!lost.quickSet(object.index()))
            notLookedAt.append(object);
    };
    for (size_t index : lookups.lostBuiltins)
        add(BuiltinID(static_cast<uint32_t>(index)));
    while (!notLookedAt.isEmpty()) {
        BuiltinID object = notLookedAt.takeLast();
        for (BuiltinID member : builtins.membersOf(object)) {
            BuiltinID found = builtins.canonicalID(member);
            PropertyKeyID key = program.keyOf(builtins.at(member).name);
            if (isObjectKind(builtins.at(found).kind) && key && lookups.namesReadThroughLostBuiltins.get(key.index()))
                add(found);
        }
        const Builtin& entry = builtins.at(object);
        if (lookups.climbsFromLostBuiltins && entry.prototypeLink == BuiltinPrototypeLink::Builtin && entry.inheritsFrom)
            add(builtins.canonicalID(entry.inheritsFrom));
    }
    return lost;
}

static Vector<NameAddedByTests> takeNamesFoundInTestRunsOfProgram(const Program& program, const ParallelLoops& loops, LinkTables& tables)
{
    Vector<NameAddedByTests> names = std::exchange(tables.namesAddedByTests, { });
    const LookupsPastProgramObjects& lookups = tables.lookups;
    if (lookups.namesWrittenThroughLostBuiltinsBesideProduct.isEmpty() || lookups.lostBuiltins.isEmpty())
        return names;
    Vector<BuiltinID> lost;
    for (size_t index : lostBuiltinsInTestRunsOfProgram(program, lookups))
        lost.append(BuiltinID(static_cast<uint32_t>(index)));
    Vector<PropertyKeyID> keys;
    for (size_t index : lookups.namesWrittenThroughLostBuiltinsBesideProduct) {
        if (program.at(PropertyKeyID(index)).kind == PropertyKeyKind::Text)
            keys.append(PropertyKeyID(index));
    }
    names.appendVector(collectInParallel<NameAddedByTests>(loops, static_cast<unsigned>(keys.size()), [&](unsigned index, Vector<NameAddedByTests>& found) {
        for (BuiltinID holder : lost) {
            if (holderTakesNameInTestRun(program, holder, keys[index]))
                found.append(NameAddedByTests { holder, program.at(keys[index]).text });
        }
    }));
    sortNamesInTestRunsOfProgram(names);
    return names;
}

void openNamesFoundByLink(Program& program, BuildContext& build, LinkTables& linked, LinkTables& held)
{
    if (!program.admitsReplacedBuiltins)
        return;
    Vector<NameAddedByTests> followed = linked.namesAddedByTests;
    followed.appendVector(held.namesAddedByTests);
    sortNamesInTestRunsOfProgram(followed);
    Vector<NameAddedByTests> opened = takeNamesFoundInTestRunsOfProgram(program, build.parallelLoops(), linked);
    Vector<NameAddedByTests> leftShut = takeNamesFoundInTestRunsOfProgram(program, build.parallelLoops(), held);
    leftShut.removeAllMatching([&](const NameAddedByTests& added) {
        return std::ranges::binary_search(opened, added);
    });
    BuiltinID objectPrototype = program.builtins().canonicalID(Builtins::ObjectPrototype);
    opened.removeAllMatching([&](const NameAddedByTests& added) {
        if (added.holder != objectPrototype)
            return false;
        leftShut.append(added);
        return true;
    });
    sortNamesInTestRunsOfProgram(leftShut);

    Vector<UserDiagnostic> diagnostics;
    for (const NameAddedByTests& added : leftShut) {
        if (std::ranges::binary_search(followed, added) && (!program.isOpenedSomewhere(added.name) || !program.isOpened(added.holder, added.name)))
            diagnostics.append(UserDiagnostic { .kind = UserDiagnosticKind::UndetectedBuiltinAddition, .text = added.name });
    }
    build.diagnostics().append(WTF::move(diagnostics));
    openNamesThatTestsAdd(program, WTF::move(opened), { });
}

static bool isWrittenByUserInTestRunsOfProgram(const Program& program, ModuleID id)
{
    const Module& module = program.at(id);
    if (module.flags.containsAny({ ModuleFlags::IsLibrary, ModuleFlags::IsBundlerHelper }))
        return false;
    switch (module.kind) {
    case ModuleKind::SourceText:
    case ModuleKind::CommonJs:
    case ModuleKind::Data:
        return true;
    case ModuleKind::RuntimeSource:
    case ModuleKind::Runtime:
    case ModuleKind::External:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static UserDiagnostic reportOfExportInTestRunsOfProgram(const Program& program, VariableID id)
{
    const Variable& variable = program.at(id);
    UserDiagnostic diagnostic { .kind = UserDiagnosticKind::UndetectedExportSpy, .text = variable.name };
    if (isWrittenByUserInTestRunsOfProgram(program, variable.module)) {
        diagnostic.location = { variable.module.index(), 0 };
        SourceLocation declaration = variable.function ? program.at(variable.function).declarationLocation() : noSourceLocation;
        if (hasModule(declaration))
            diagnostic.location = declaration;
    }
    return diagnostic;
}

void finishFactsOfTestRun(const Program& program, BuildContext& build, FactTables& tables, Vector<VariableID>&& exportsSpiedOnThatWireLeavesShut)
{
    if (!program.admitsReplacedBuiltins)
        return;

    for (const BuiltinLeftOpen& open : tables.builtinMembersLeftOpen) {
        if (open.reason != ReasonBuiltinStaysOpen::IsOpenToDebugger && !program.builtins().has(BuiltinMark::MayBeAbsent, open.member))
            tables.builtinMembersBehindWatch.set(open.member.index());
        if (!program.isLockLeftToProgram(open.member))
            continue;
        tables.builtinMembersThatTestsReplace.append(open.member);
    }
    RELEASE_ASSERT(std::ranges::is_sorted(tables.builtinMembersThatTestsReplace));
    for (BuiltinID member : program.builtinsWhoseLockIsLeftToProgram()) {
        bool isLocked = tables.builtinMembersLockedAtStartUp.get(member.index());
        RELEASE_ASSERT(isLocked || !tables.constantBuiltinMembers.get(member.index()));
        RELEASE_ASSERT(!isLocked || !std::ranges::binary_search(tables.builtinMembersThatTestsReplace, member));
    }

    Vector<VariableID> exports = WTF::move(exportsSpiedOnThatWireLeavesShut);
    AOT::sort(exports);
    removeRepeatedElements(exports);
    Vector<UserDiagnostic> diagnostics;
    for (VariableID variable : exports)
        diagnostics.append(reportOfExportInTestRunsOfProgram(program, variable));
    build.diagnostics().append(WTF::move(diagnostics));
}

bool isFileOfProductInTestRun(const Program& program, FunctionID writer)
{
    if (!program.admitsReplacedBuiltins || !writer)
        return false;
    ModuleID module = program.at(writer).module;
    return module && program.at(module).flags.contains(ModuleFlags::HasReadOnlyBuiltins);
}

bool writeOpensNothingInTestRun(const Program& program, FunctionID writer, BuiltinID member)
{
    return isFileOfProductInTestRun(program, writer) && program.isLockLeftToProgram(member);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
