/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTThreads.h"
#include <atomic>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

// What is expected of a position is a guess. A list of a few layouts is a good one: the implementers of an interface, in a file that is not checked. A long list is what the uses of a value say
// that the checker calls `any`: `props.value`: every layout of the program with a `value`. It tells nothing: the object that comes is as often none of them, the test fails, and what stands
// behind the test goes by name with it.
class LongExpectations {
public:
    LongExpectations(const Program& program, const ParallelLoops& loops)
        : m_listsTooMany(program.types.size())
    {
        constexpr size_t mostLayouts = 16;
        constexpr size_t typesPerTask = 1024;
        std::atomic<bool> hasAny { false };
        forEachIndexInParallel(loops, static_cast<unsigned>((m_listsTooMany.size() + typesPerTask - 1) / typesPerTask), [&](unsigned task) {
            for (size_t index = task * typesPerTask, end = std::min(m_listsTooMany.size(), index + typesPerTask); index < end; ++index) {
                const Type& type = program.at(TypeID(static_cast<uint32_t>(index)));
                m_listsTooMany[index] = type.layouts && membersOf(program, type).size() > mostLayouts;
                if (m_listsTooMany[index])
                    hasAny.store(true, std::memory_order_relaxed);
            }
        });
        m_hasAny = hasAny.load(std::memory_order_relaxed);
    }

    bool hasAny() const { return m_hasAny; }

    bool forget(DeclaredType& declared) const
    {
        if (!declared.flags.contains(PositionFlags::IsExpected) || !declared.type || !m_listsTooMany[declared.type.index()])
            return false;
        declared.type = TypeTable::anything;
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
    Vector<uint8_t> m_listsTooMany;
    bool m_hasAny { false };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
