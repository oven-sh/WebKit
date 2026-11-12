/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCoroutineWords.h"
#include "AOTWatchpoints.h"
#include <wtf/FastMalloc.h>
#include <array>
#include <wtf/Noncopyable.h>

namespace JSC {

class JSObject;
class Structure;
class VM;

namespace AOT {

struct Instance;
struct State;

class CoroutineTable {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(CoroutineTable);
    WTF_MAKE_NONCOPYABLE(CoroutineTable);
public:
    explicit CoroutineTable(State&);

    Structure* generatorStructure() const { return m_generatorStructure; }
    Structure* savedFrameStructure() const { return m_savedFrameStructure; }
    Structure* reactionChainStructure(VM&);
    Structure* structureOfGeneratorsOfLibrary(VM&, bool isAsync);
    void* savedFrameEntrypoint() const { return m_savedFrameEntrypoint; }
    void* handlerOfJobEntrypoint() const { return m_handlerOfJobEntrypoint; }

    template<typename Visitor> void visitRoots(Visitor&);

private:
    uint32_t createEmptyPrototypeStructure(VM&, JSObject* parent);
    void initializeCoroutineWords(VM&);

    State& m_state;
    Instance& m_instance;
    void* m_savedFrameEntrypoint;
    void* m_handlerOfJobEntrypoint;
    Structure* m_generatorStructure { nullptr };
    Structure* m_savedFrameStructure { nullptr };
    Structure* m_reactionChainStructure { nullptr };
    std::array<std::unique_ptr<PropertyValueWatchpoint>, 2> m_watchesAboveGeneratorFunctionsOfLibrary;
    std::array<bool, 2> m_hasAskedAboveGeneratorFunctionsOfLibrary { };
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
