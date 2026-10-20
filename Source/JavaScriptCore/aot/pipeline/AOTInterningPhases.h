/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include "AOTProvisionalFunctions.h"
#include "AOTStringArena.h"
#include "AOTTypeArena.h"
#include <span>
#include <wtf/Noncopyable.h>
#include <wtf/Nonmovable.h>

namespace JSC { namespace AOT { namespace Typed {

struct BodyRemaps {
    bool isIdentity() const { return types.isIdentity() && strings.isIdentity() && functions.isIdentity(); }

    TypeRemap types;
    StringRemap strings;
    FunctionRemap functions;
};

class InterningPhases {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(InterningPhases);
    WTF_MAKE_NONCOPYABLE(InterningPhases);
    WTF_MAKE_NONMOVABLE(InterningPhases);
public:
    InterningPhases(Program&, unsigned numberOfTasks);
    InterningPhases(ProgramEditor&, unsigned numberOfTasks);

    unsigned addTasks(unsigned count);

    BodyRemaps finish();
    BodyRemaps finish(std::span<const unsigned> taskOrder, FunctionRemap&&);
    void discard();

private:
    friend class TaskArenas;

    TypeInterningPhase m_types;
    StringInterningPhase m_strings;
};

class TaskArenas {
    WTF_MAKE_NONCOPYABLE(TaskArenas);
    WTF_MAKE_NONMOVABLE(TaskArenas);
public:
    explicit TaskArenas(Program&);
    TaskArenas(InterningPhases&, unsigned task);

    TypeArena types;
    StringArena strings;
};

void applyRemaps(const BodyRemaps&, Function&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
