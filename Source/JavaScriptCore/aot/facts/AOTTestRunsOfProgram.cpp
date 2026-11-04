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

// ---- Mocks, by the records alone

// namespacesThatMocksMayStandFor() comes here in front of the check of the records, which reports an id outside its list.
static bool namesModuleInTestRunsOfProgram(const Program& program, ModuleID id)
{
    return id && id.index() < program.modules.size();
}

// A mock of a CommonJS file stands in place of the file. What the file exports are variables of its face. False: no module has the flag, and the set is left as it was.
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
        if (mocked.quickGet(module.requests[0].target.index()) && isFaceOfCommonJSFile(program, module))
            mocked.quickSet(index);
    }
    return true;
}

// `export * as inner from "./inner"`: a mock that is laid over a module that hands the object on gives what it likes in its place, to every module that hands it on.
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

// Backwards over the requests, from the modules that may be mocked. Each module is taken once, and each request is gone along once.
static BitVector modulesThatLeadToMocksInTestRunsOfProgram(const Program& program, const BitVector& mocked)
{
    size_t numberOfModules = program.modules.size();
    // By target, and one more: where those who request it begin.
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
    mocks.modulesThatLeadToMocks = modulesThatLeadToMocksInTestRunsOfProgram(program, mocks.modulesThatMayBeMocked);
    return mocks;
}

bool mayComeThroughMock(const Program& program, const MocksOfProgram& mocks, ModuleID exporter, uint32_t indexOfExport)
{
    if (!mocks.isAnyModuleMocked)
        return false;
    // The names of a mock that comes ahead of the load are the names of the module: those that its factory gives, and no other.
    if (!exporter || mocks.modulesThatMayBeMocked.get(exporter.index()))
        return true;
    // `export * from "./lib"`, and a mock of lib whose factory gives a name more: the barrel has it too.
    const auto& exports = program.at(exporter).exports;
    if (indexOfExport >= exports.size())
        return mocks.modulesThatLeadToMocks.get(exporter.index());

    const ModuleExport& entry = exports[indexOfExport];
    switch (entry.kind) {
    case ModuleExportKind::Variable:
        // Program::appendNamespaceLayouts() asks in front of the check of the records.
        if (!entry.variable || entry.variable.index() >= program.variables.size())
            return true;
        // `export { sides } from "./shapes"`: the entry names the variable of the module that declares it. A mock may stand in place of a module that hands it on, or of the one that declares it,
        // and answers for whoever has the name through it. A module lists what somebody reads off its object, so one on the way may lack the name: any mock that the requests lead to may stand on it.
        if (program.at(entry.variable).module != exporter && mocks.modulesThatLeadToMocks.get(exporter.index()))
            return true;
        return false;
    case ModuleExportKind::Namespace:
        if (!entry.module)
            return true;
        // Under a mock that module may have two objects. Which of them this one hands on goes by when it was linked, and the run time knows that.
        if (mocks.modulesThatMayBeMocked.get(entry.module.index()))
            return true;
        if (mocks.namespacesHandedOnByMockedModules.get(entry.module.index()))
            return true;
        return false;
    // Nobody here follows the name from module to module.
    case ModuleExportKind::Indirect:
        return mocks.isAnyModuleMocked;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

BitVector namespacesThatMocksMayStandFor(const Program& program)
{
    BitVector mocked;
    if (!markModulesThatMayBeMockedInTestRunsOfProgram(program, mocked))
        return mocked;
    BitVector all = namespacesHandedOnInTestRunsOfProgram(program, mocked);
    all.merge(mocked);
    return all;
}

// ---- Names that tests add to built-ins

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

// LookupsPastProgramObjects::lostBuiltins, what the reads through such values find on them, and what stands above them where somebody climbs: as WritesOnBuiltinsInProgramFacts::closeOverNamesReadThroughLostBuiltins().
// Each object is looked at once.
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

// `for (const [, place] of places()) Reflect.set(place, "mark", 1)`: the flow brings several, and the write counts on each.
static Vector<NameAddedByTests> takeNamesFoundInTestRunsOfProgram(const Program& program, LinkTables& tables)
{
    Vector<NameAddedByTests> names = std::exchange(tables.namesAddedByTests, { });
    const LookupsPastProgramObjects& lookups = tables.lookups;
    if (lookups.namesWrittenThroughLostBuiltins.isEmpty() || lookups.lostBuiltins.isEmpty())
        return names;
    BitVector lost = lostBuiltinsInTestRunsOfProgram(program, lookups);
    for (size_t keyIndex : lookups.namesWrittenThroughLostBuiltins) {
        PropertyKeyID key(keyIndex);
        if (program.at(key).kind != PropertyKeyKind::Text)
            continue;
        for (size_t holderIndex : lost) {
            BuiltinID holder(static_cast<uint32_t>(holderIndex));
            if (holderTakesNameInTestRun(program, holder, key))
                names.append(NameAddedByTests { holder, program.at(key).text });
        }
    }
    sortNamesInTestRunsOfProgram(names);
    return names;
}

void openNamesFoundByLink(Program& program, BuildContext& build, LinkTables& linked, LinkTables& held)
{
    if (!program.admitsReplacedBuiltins)
        return;
    Vector<NameAddedByTests> opened = takeNamesFoundInTestRunsOfProgram(program, linked);
    Vector<NameAddedByTests> leftShut = takeNamesFoundInTestRunsOfProgram(program, held);
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
        if (!program.isOpenedSomewhere(added.name) || !program.isOpened(added.holder, added.name))
            diagnostics.append(UserDiagnostic { .kind = UserDiagnosticKind::NameAddedToBuiltinWasNotOpened, .text = added.name });
    }
    build.diagnostics().append(WTF::move(diagnostics));
    openNamesThatTestsAdd(program, WTF::move(opened), { });
}

// ---- Behind the facts of the program

// A diagnostic that lies in Bun's own code is taken for a fault of the compiler.
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

// The front end has folded by what it opened. What Link alone finds is open nowhere: the door refuses the spy aloud.
static UserDiagnostic reportOfExportInTestRunsOfProgram(const Program& program, VariableID id)
{
    const Variable& variable = program.at(id);
    UserDiagnostic diagnostic { .kind = UserDiagnosticKind::SpiedExportWasNotOpened, .text = variable.name };
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

    // The run time goes by its own table, which locks each of these, and by the list of the image.
    for (const BuiltinLeftOpen& open : tables.builtinMembersLeftOpen) {
        if (program.isLockLeftToProgram(open.member))
            tables.builtinMembersThatTestsReplace.append(open.member);
    }
    RELEASE_ASSERT(std::ranges::is_sorted(tables.builtinMembersThatTestsReplace));
    // Code takes a member for a constant only where the program lays the lock, and no member that the image opens has one.
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

// ---- Files of the product

bool isFileOfProductInTestRun(const Program& program, FunctionID writer)
{
    if (!program.admitsReplacedBuiltins || !writer)
        return false;
    ModuleID module = program.at(writer).module;
    return module && program.at(module).flags.contains(ModuleFlags::FindsBuiltinsReadOnly);
}

bool writeOpensNothingInTestRun(const Program& program, FunctionID writer, BuiltinID member)
{
    return isFileOfProductInTestRun(program, writer) && program.isLockLeftToProgram(member);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
