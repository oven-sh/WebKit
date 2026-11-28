/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTModules.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTRuntimeReporter.h"
#include "AOTVMState.h"
#include "JSCInlines.h"
#include "Options.h"
#include <array>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

namespace JSC { namespace AOT {

auto Modules::mockState(ModuleID module) -> MockState
{
    LoadedModule* loaded = loadedIfExists(module);
    bool waits = m_modulesWithPendingMock.get(module.index());
    bool isInPlace = m_modulesWithActiveMock.get(module.index());
    bool isLaidOver = loaded && loaded->isPatchedByMock;
    bool isOverModuleThatHasRun = hasIntactOriginal(module);
    bool isToBeLetGo = hasRemovedMock(module);
    bool hasWaited = m_modulesWithDeferredMock.get(module.index());
    bool hasKeptError = m_modulesWithMockLoadError.get(module.index()) || m_modulesWithCachedMockError.get(module.index());
    bool isOfLibrary = data(module).isOfLibrary();
    ModuleStatus body = status(module);
    bool hasBegun = body != ModuleStatus::Unlinked && body != ModuleStatus::Linking && body != ModuleStatus::Linked;
    if (isLaidOver && (waits || isInPlace))
        return MockState::NoneOfThem;
    if (isToBeLetGo)
        return !isInPlace ? MockState::NoneOfThem : waits ? MockState::EndedWhileAnotherWaits : MockState::Ended;
    if (waits && isInPlace && !isOverModuleThatHasRun)
        return isMocked(module) ? MockState::InPlaceWhileAnotherWaits : MockState::NoneOfThem;
    if (waits) {
        if ((!isMocked(module) && !isOfLibrary) || (hasBegun && !isOverModuleThatHasRun && !isOfLibrary && data(module).kind != ModuleKind::CommonJs))
            return MockState::NoneOfThem;
        return hasKeptError ? MockState::Failed : MockState::Waits;
    }
    if (isInPlace) {
        if (isOverModuleThatHasRun)
            return MockState::InPlaceOverModuleThatHasRun;
        return body == ModuleStatus::Evaluating || body == ModuleStatus::EvaluatingAsync ? MockState::NoneOfThem : MockState::InPlace;
    }
    if (isOfLibrary && m_libraryModulesWithMock.get(module.index()))
        return m_libraryModulesThatMockReplaces.get(module.index()) ? MockState::InPlace : MockState::LaidOver;
    if (isLaidOver)
        return hasBegun || body == ModuleStatus::Linked ? MockState::LaidOver : MockState::NoneOfThem;
    if (isOverModuleThatHasRun)
        return MockState::Runs;
    if (isMocked(module))
        return hasWaited || isOfLibrary || m_modulesWithRunningFactory.get(module.index()) ? MockState::Runs : MockState::Fills;
    if (hasKeptError)
        return MockState::Failed;
    return hasWaited ? MockState::NoneOfThem : MockState::None;
}

String Modules::describeMockState(ModuleID module)
{
    static constexpr std::array<ASCIILiteral, 12> names { "none"_s, "waits"_s, "runs"_s, "in place"_s, "in place while another waits"_s, "in place over a module that has run"_s, "fills"_s, "laid over"_s, "failed"_s, "ended"_s, "ended while another waits"_s, "NONE OF THEM"_s };
    static constexpr std::array<ASCIILiteral, 7> bodies { "unlinked"_s, "linking"_s, "linked"_s, "evaluating"_s, "awaiting"_s, "evaluated"_s, "failed"_s };
    MockState state = mockState(module);
    ASCIILiteral kind = data(module).isOfLibrary() ? "library"_s : data(module).kind == ModuleKind::CommonJs ? "CommonJS"_s : data(module).isFacade() ? "facade"_s : data(module).isESModuleOfProgram() ? "ES"_s : "other"_s;
    StringBuilder text;
    text.append(kind, ' ', names[static_cast<size_t>(state)], " (body "_s, bodies[static_cast<size_t>(status(module)) - static_cast<size_t>(ModuleStatus::Unlinked)], m_originalsBeingEvaluated.get(module.index()) ? ", its original runs"_s : ""_s, isEvicted(module) ? ", left to the host)"_s : ")"_s);
    if (state == MockState::NoneOfThem) {
        LoadedModule* loaded = loadedIfExists(module);
        auto say = [&](bool isSet, ASCIILiteral what) {
            if (isSet)
                text.append(' ', what);
        };
        text.append(" ["_s);
        say(isMocked(module), "mocked"_s);
        say(m_modulesWithPendingMock.get(module.index()), "to-run"_s);
        say(m_modulesWithDeferredMock.get(module.index()), "waited"_s);
        say(m_mocksRetriedAfterFailure.get(module.index()), "asks-again"_s);
        say(m_modulesWithActiveMock.get(module.index()), "in-place"_s);
        say(hasRemovedMock(module), "to-let-go"_s);
        say(loaded && loaded->isPatchedByMock, "laid-over"_s);
        say(hasIntactOriginal(module), "over-run"_s);
        say(m_modulesWithMockLoadError.get(module.index()), "load-ended"_s);
        say(m_modulesWithCachedMockError.get(module.index()), "error-kept"_s);
        text.append(" ]"_s);
    }
    return text.toString();
}

static void countMockTransitionInTestRunMockStates(VM& vm, const String& transition)
{
    if (RuntimeCounts* counts = vm.aotState().counts.get())
        counts->didPassMockDoor(transition);
}

void Modules::countMockStates(ASCIILiteral door, const String& before, ModuleID module)
{
    String after = describeMockState(module);
    countMockTransitionInTestRunMockStates(*m_instance.vm, makeString(before, " > "_s, door, " > "_s, after));
    if (mockState(module) == MockState::NoneOfThem) [[unlikely]]
        writeToStandardError(makeString("[aot] the mock of "_s, m_table.fullPath(module), " is in no state of its life: "_s, before, " > "_s, door, " > "_s, after, '\n'));
}

MockDoor::MockDoor(Modules* modules, ModuleID module, ASCIILiteral door)
    : m_modulesOfMock(modules)
    , m_modules(Options::aotCountsPath() ? modules : nullptr)
    , m_module(module)
    , m_door(door)
{
    if (modules)
        modules->dropAnswersKept();
    if (m_modules) [[unlikely]]
        m_before = m_modules->describeMockState(module);
}

MockDoor::~MockDoor()
{
    if (m_modulesOfMock)
        m_modulesOfMock->didChangeLinkState();
    if (m_modules) [[unlikely]]
        m_modules->countMockStates(m_door, m_before, m_module);
}

void countMockCall(JSGlobalObject* realm, std::optional<ModuleID> module, ASCIILiteral otherwise, ASCIILiteral call, ASCIILiteral factory)
{
    if (!Options::aotCountsPath()) [[likely]]
        return;
    if (!module) {
        countMockTransitionInTestRunMockStates(realm->vm(), makeString("no compiled module ("_s, otherwise, ") x "_s, call, ", "_s, factory));
        return;
    }
    if (Modules* modules = modulesOf(realm))
        countMockTransitionInTestRunMockStates(realm->vm(), makeString(modules->describeMockState(*module), " x "_s, call, ", "_s, factory));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
