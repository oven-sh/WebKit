/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTVector.h"
#include "CollectionScope.h"
#include <wtf/FastMalloc.h>
#include <array>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>

namespace JSC {

class JSObject;
class Structure;
class VM;

namespace AOT {

class SavedFrame;
struct Instance;
struct State;

class CoroutineTable {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(CoroutineTable);
    WTF_MAKE_NONCOPYABLE(CoroutineTable);
public:
    explicit CoroutineTable(State&);

    Structure* generatorStructure() const { return m_generatorStructure; }
    Structure* savedFrameStructure() const { return m_savedFrameStructure; }
    void* savedFrameEntrypoint() const { return m_savedFrameEntrypoint; }
    void* handlerOfJobEntrypoint() const { return m_handlerOfJobEntrypoint; }

    void didCreateSavedFrame(VM&, SavedFrame*);
    // From any thread that marks.
    void didVisitFrameWithDeadWords(SavedFrame*, uint32_t version, const uint32_t* firstWordOfMap);

    template<typename Visitor> void visitRoots(Visitor&);
    size_t removeDeadEntries(VM&, CollectionScope);

private:
    struct VisitedFrameWithDeadWords {
        SavedFrame* frame;
        const uint32_t* firstWordOfMap;
        uint32_t version;
    };
    // Threads that mark at one time take different lists, and no two lists share a cache line.
    struct ListOfVisitedFrames {
        Lock lock;
        Vector<VisitedFrameWithDeadWords> frames WTF_GUARDED_BY_LOCK(lock);
        std::array<uint8_t, 104> padding;
    };
    static_assert(sizeof(ListOfVisitedFrames) == 128);

    uint32_t createEmptyPrototypeStructure(VM&, JSObject* parent);
    void initializeCoroutineWords(VM&);

    State& m_state;
    Instance& m_instance;
    void* m_savedFrameEntrypoint;
    void* m_handlerOfJobEntrypoint;
    Structure* m_generatorStructure { nullptr };
    Structure* m_savedFrameStructure { nullptr };
    Vector<SavedFrame*> m_savedFramesForValidation;
    std::array<ListOfVisitedFrames, 16> m_visitedFramesWithDeadWords;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
