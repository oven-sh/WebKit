/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <atomic>

namespace JSC { namespace AOT { namespace Typed {

// What is expected of a position is a guess. That a location holds nothing until its first store is none, so the mark of a dead zone stays.
class LongExpectations {
public:
    LongExpectations(const Program& program, const ParallelLoops& loops)
        : m_types(program.types)
        , m_isLong(program.types.size())
    {
        constexpr size_t mostLayouts = 16;
        constexpr size_t typesPerTask = 1024;
        std::atomic<bool> hasAny { false };
        forEachIndexInParallel(loops, static_cast<unsigned>((m_isLong.size() + typesPerTask - 1) / typesPerTask), [&](unsigned task) {
            for (size_t index = task * typesPerTask, end = std::min(m_isLong.size(), index + typesPerTask); index < end; ++index) {
                const Type& type = program.at(TypeID(static_cast<uint32_t>(index)));
                m_isLong[index] = type.layouts && membersOf(program, type).size() > mostLayouts;
                if (m_isLong[index])
                    hasAny.store(true, std::memory_order_relaxed);
            }
        });
        m_hasAny = hasAny.load(std::memory_order_relaxed);
    }

    bool hasAny() const { return m_hasAny; }

    bool forget(DeclaredType& declared) const
    {
        if (!declared.flags.contains(PositionFlags::IsExpected) || !declared.type || !m_isLong[declared.type.index()])
            return false;
        declared.type = m_types.provenTypeOf(declared);
        declared.flags.remove(PositionFlags::IsExpected);
        return true;
    }

    void forgetInRecordOf(Function& function) const
    {
        for (DeclaredType& declared : function.parameters)
            forget(declared);
        for (DeclaredType& declared : function.results)
            forget(declared);
        forget(function.thisType);
    }

    void forgetInBodyOf(Function& function) const
    {
        for (Value& value : function.editAllValues()) {
            if (value.opcode != Opcode::Convert)
                continue;
            DeclaredType declared = value.declared();
            if (forget(declared))
                value.setDeclared(declared);
        }
    }

private:
    const TypeTable& m_types;
    Vector<bool> m_isLong;
    bool m_hasAny { false };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
