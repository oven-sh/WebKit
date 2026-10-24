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
    LongExpectations(Program& program, const ParallelLoops& loops)
        : m_lists(program.types.size())
    {
        constexpr size_t mostLayouts = 16;
        constexpr size_t typesPerTask = 1024;
        std::atomic<bool> hasAny { false };
        std::atomic<bool> hasAnyWithDeadZone { false };
        forEachIndexInParallel(loops, static_cast<unsigned>((m_lists.size() + typesPerTask - 1) / typesPerTask), [&](unsigned task) {
            for (size_t index = task * typesPerTask, end = std::min(m_lists.size(), index + typesPerTask); index < end; ++index) {
                const Type& type = program.at(TypeID(static_cast<uint32_t>(index)));
                bool listsTooMany = type.layouts && membersOf(program, type).size() > mostLayouts;
                bool hasDeadZone = type.primitives & Primitive::Absent;
                m_lists[index] = !listsTooMany ? List::Short : hasDeadZone ? List::LongWithDeadZone : List::Long;
                if (listsTooMany)
                    hasAny.store(true, std::memory_order_relaxed);
                if (listsTooMany && hasDeadZone)
                    hasAnyWithDeadZone.store(true, std::memory_order_relaxed);
            }
        });
        m_hasAny = hasAny.load(std::memory_order_relaxed);
        if (hasAnyWithDeadZone.load(std::memory_order_relaxed))
            m_anythingWithDeadZone = program.types.intern(Type::anything().withPrimitives(Primitive::Absent));
    }

    bool hasAny() const { return m_hasAny; }

    bool forget(DeclaredType& declared) const
    {
        if (!declared.flags.contains(PositionFlags::IsExpected) || !declared.type || m_lists[declared.type.index()] == List::Short)
            return false;
        declared.type = m_lists[declared.type.index()] == List::LongWithDeadZone ? m_anythingWithDeadZone : TypeTable::anything;
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
    enum class List : uint8_t { Short, Long, LongWithDeadZone };

    Vector<List> m_lists;
    TypeID m_anythingWithDeadZone;
    bool m_hasAny { false };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
