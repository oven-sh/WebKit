/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

#include "AirInst.h"
#include <array>

namespace JSC { namespace B3 { namespace Air {

template<typename... Arguments>
inline bool isExtendedForm(Opcode opcode, Arguments... arguments)
{
    constexpr size_t size = sizeof...(Arguments);
    const std::array<Arg::Kind, size + 3> kinds { arguments..., Arg::Invalid, Arg::Invalid, Arg::Invalid };
    switch (opcode) {
    case Mul64:
        return size == 3 && kinds[0] == Arg::Imm;
    case SignExtend32To64:
        return size == 2 && kinds[0] != Arg::Tmp;
    case Move:
        return size == 2 && kinds[0] == Arg::Imm && kinds[1] == Arg::Index;
    case Test64:
    case MoveConditionallyTest64:
        return kinds[1] == Arg::Addr;
    case Branch32:
        return kinds[2] == Arg::Index || (kinds[1] == Arg::Index && kinds[2] == Arg::Tmp);
    case Branch64:
        return kinds[2] == Arg::Index || (kinds[1] == Arg::Index && kinds[2] == Arg::Imm);
    default:
        return false;
    }
}

inline bool admitsStackOnlyInExtendedForm(const Inst& inst, unsigned argIndex)
{
    switch (inst.kind.opcode) {
    case SignExtend32To64:
        return !argIndex;
    case Test64:
    case MoveConditionallyTest64:
        return argIndex == 1;
    default:
        return false;
    }
}

} } } // namespace JSC::B3::Air

#endif // ENABLE(B3_JIT)
