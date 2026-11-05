/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTEffects.h"
#include <array>

namespace JSC { namespace AOT { namespace Typed {

enum class KindOfEffectsRow : uint8_t { None, Operation, RuntimeOperation, Stub, Builtin, Callee };
enum class PartOfEffects : uint8_t { None, RunsOtherCode, Throws, Writes, Collects };

inline constexpr std::array<AbstractHeap, 8> heapsOfDigestInEffectsNet {
    AbstractHeap::ShapeWords, AbstractHeap::OrderWords, AbstractHeap::ObjectFields, AbstractHeap::NamedProperties,
    AbstractHeap::ArrayStorage, AbstractHeap::Elements, AbstractHeap::StringStorage, AbstractHeap::TypedArrayStorage,
};
constexpr uint8_t lanesNotWrittenInEffectsNet(const Effects& effects)
{
    uint8_t lanes = 0;
    for (unsigned lane = 0; lane < heapsOfDigestInEffectsNet.size(); ++lane) {
        if (!effects.writes.contains(heapsOfDigestInEffectsNet[lane]))
            lanes |= static_cast<uint8_t>(1u << lane);
    }
    return lanes;
}

inline constexpr unsigned firstRowOfOperationsInEffectsNet = 0;
inline constexpr unsigned firstRowOfRuntimeOperationsInEffectsNet = 2048;
inline constexpr unsigned firstRowOfStubsInEffectsNet = 6144;
inline constexpr unsigned firstRowOfBuiltinsInEffectsNet = 8192;
inline constexpr unsigned numberOfRowsInEffectsNet = 24576;
inline constexpr unsigned noRowInEffectsNet = UINT16_MAX;
inline constexpr unsigned bytesOfRowInEffectsNet = 4;
constexpr unsigned byteInEffectsNet(unsigned row, PartOfEffects part) { return row * bytesOfRowInEffectsNet + static_cast<unsigned>(part) - 1; }

struct RowThatSaysLess {
    KindOfEffectsRow kind { KindOfEffectsRow::None };
    PartOfEffects part { PartOfEffects::None };
    AbstractHeap heap { };
    uint16_t row { 0 };

    static constexpr RowThatSaysLess decode(uint32_t word)
    {
        return { static_cast<KindOfEffectsRow>(word >> 28), static_cast<PartOfEffects>(word >> 24 & 0xf), static_cast<AbstractHeap>(word >> 16 & 0xff), static_cast<uint16_t>(word) };
    }
    constexpr uint32_t encode() const { return static_cast<uint32_t>(kind) << 28 | static_cast<uint32_t>(part) << 24 | static_cast<uint32_t>(heap) << 16 | row; }

    void cut(KindOfEffectsRow kindOfRow, unsigned number, Effects& effects) const
    {
        if (kind != kindOfRow || row != number)
            return;
        switch (part) {
        case PartOfEffects::None:
            return;
        case PartOfEffects::RunsOtherCode:
            effects.mayCallUnknownCode = false;
            return;
        case PartOfEffects::Throws:
            effects.mayThrow = false;
            return;
        case PartOfEffects::Writes:
            effects.writes.remove(AbstractHeapSet { heap });
            return;
        case PartOfEffects::Collects:
            effects.mayCollect = false;
            return;
        }
    }
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
