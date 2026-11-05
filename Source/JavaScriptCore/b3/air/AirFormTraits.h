/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

#include "AirArg.h"
#include "AirFormTable.h"
#include <array>
#include <wtf/Vector.h>

namespace JSC { namespace B3 { namespace Air {

namespace Greedy {

struct Mentions {
    static constexpr unsigned earlyUse = 1;
    static constexpr unsigned lateUse = 2;
    static constexpr unsigned earlyDef = 4;
    static constexpr unsigned lateDef = 8;
    static constexpr unsigned timingMask = 15;
    static constexpr unsigned wasDecodedSlowly = 16;
    static constexpr unsigned anyUse = 16;
    static constexpr unsigned zDef = 32;
    static constexpr unsigned anyDef = 64;
    static constexpr unsigned warmUseOrDef = 128;
    static constexpr unsigned coldUse = 256;
    static constexpr unsigned widthShift = 9;
    static constexpr unsigned widthMask = 7;
    static constexpr unsigned isFP = 1 << 12;
    static constexpr unsigned hasNoWidthInformation = 1 << 13;
    static constexpr unsigned isDefByConstant = 1 << 14;
    static constexpr unsigned numberOfRoles = 12;

    static constexpr unsigned traitsOf(Arg::Role role)
    {
        switch (role) {
        case Arg::Use:
            return earlyUse | anyUse | warmUseOrDef;
        case Arg::ColdUse:
            return earlyUse | anyUse | coldUse;
        case Arg::LateUse:
            return lateUse | anyUse | warmUseOrDef;
        case Arg::LateColdUse:
            return lateUse | anyUse | coldUse;
        case Arg::Def:
            return lateDef | anyDef | warmUseOrDef;
        case Arg::ZDef:
            return lateDef | anyDef | zDef | warmUseOrDef;
        case Arg::UseDef:
            return earlyUse | lateDef | anyUse | anyDef | warmUseOrDef;
        case Arg::UseZDef:
            return earlyUse | lateDef | anyUse | anyDef | zDef | warmUseOrDef;
        case Arg::EarlyDef:
            return earlyDef | anyDef | warmUseOrDef;
        case Arg::EarlyZDef:
            return earlyDef | anyDef | zDef | warmUseOrDef;
        case Arg::Scratch:
            return earlyDef | lateUse | anyUse | anyDef | warmUseOrDef;
        case Arg::UseAddr:
            return 0;
        }
        return 0;
    }

    static constexpr unsigned traitsOf(Arg::Role role, Bank bank, Width width)
    {
        return traitsOf(role) | static_cast<unsigned>(width) << widthShift | (bank == FP ? isFP : 0);
    }

    static constexpr std::array<uint16_t, 256> computeFormTraits()
    {
        std::array<uint16_t, 256> result { };
        for (unsigned form = 0; form < 256; ++form) {
            unsigned role = (form >> formRoleShift) & formRoleMask;
            unsigned width = (form >> formWidthShift) & formWidthMask;
            if (role >= numberOfRoles || !width || width > 5)
                continue;
            result[form] = traitsOf(static_cast<Arg::Role>(role), static_cast<Bank>((form >> formBankShift) & formBankMask), static_cast<Width>(width - 1));
        }
        return result;
    }

    Vector<unsigned> tmps;
    Vector<uint16_t> traits;
    Vector<unsigned> instFirstMention;
    Vector<uint8_t> instTimings;
    unsigned numberOfInsts { 0 };
};

static constexpr std::array<uint16_t, 256> formTraits = Mentions::computeFormTraits();

} // namespace Greedy

template<typename Functor> // functor(Tmp, unsigned traits)
ALWAYS_INLINE void forEachTmpWithTraits(const Arg& arg, unsigned argTraits, NOESCAPE const Functor& functor)
{
    using Greedy::Mentions;
    auto traitsInAddress = [&](Arg::Role role) ALWAYS_INLINE_LAMBDA -> unsigned {
        bool isUseAddr = !(argTraits & (Mentions::anyUse | Mentions::anyDef));
        unsigned width = isUseAddr ? (argTraits >> Mentions::widthShift) & Mentions::widthMask : static_cast<unsigned>(pointerWidth());
        return Mentions::traitsOf(role) | width << Mentions::widthShift;
    };
    switch (arg.kind()) {
    case Arg::Tmp:
        functor(arg.tmp(), argTraits);
        return;
    case Arg::SimpleAddr:
    case Arg::Addr:
    case Arg::ExtendedOffsetAddr:
        functor(arg.base(), traitsInAddress(Arg::Use));
        return;
    case Arg::PreIndex:
    case Arg::PostIndex:
        functor(arg.base(), traitsInAddress(Arg::UseDef));
        return;
    case Arg::Index:
        functor(arg.base(), traitsInAddress(Arg::Use));
        functor(arg.index(), traitsInAddress(Arg::Use));
        return;
    case Arg::Invalid:
    case Arg::Imm:
    case Arg::BigImm:
    case Arg::BitImm:
    case Arg::BitImm64:
    case Arg::FPImm32:
    case Arg::FPImm64:
    case Arg::FPImm128:
    case Arg::Stack:
    case Arg::CallArg:
    case Arg::RelCond:
    case Arg::ResCond:
    case Arg::DoubleCond:
    case Arg::StatusCond:
    case Arg::Special:
    case Arg::WidthArg:
    case Arg::ZeroReg:
    case Arg::SIMDInfo:
        return;
    }
}

} } } // namespace JSC::B3::Air

#endif // ENABLE(B3_JIT)
