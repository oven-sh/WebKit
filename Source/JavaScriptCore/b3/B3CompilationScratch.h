/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

#include "B3Arena.h"
#include <memory>
#include <wtf/Noncopyable.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/Vector.h>

namespace JSC { namespace B3 {

class AbstractHeapRepository;

class CompilationScratch {
    WTF_MAKE_NONCOPYABLE(CompilationScratch);
    WTF_MAKE_TZONE_ALLOCATED(CompilationScratch);
public:
    using HeapRepositoryFactory = std::unique_ptr<AbstractHeapRepository> (*)();

    JS_EXPORT_PRIVATE explicit CompilationScratch(HeapRepositoryFactory = nullptr);
    JS_EXPORT_PRIVATE ~CompilationScratch();

    JS_EXPORT_PRIVATE void setHeapRepositoryFactory(HeapRepositoryFactory);

private:
    friend class Arena;
    friend class Procedure;

    static constexpr unsigned maxRetainedRepositories = 16;
    static constexpr unsigned maxRetainedChunks = 16;

    std::unique_ptr<AbstractHeapRepository> createHeaps();
    std::unique_ptr<AbstractHeapRepository> takeHeaps();
    void giveBackHeaps(std::unique_ptr<AbstractHeapRepository>&&);

    Arena::Chunk* takeFirstChunk();
    void giveBackFirstChunk(Arena::Chunk*);

    HeapRepositoryFactory m_createHeaps { nullptr };
    Vector<std::unique_ptr<AbstractHeapRepository>, 1> m_heaps;
    Vector<Arena::Chunk*, 1> m_firstChunks;
    unsigned m_numberOfUsers { 0 };
};

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
