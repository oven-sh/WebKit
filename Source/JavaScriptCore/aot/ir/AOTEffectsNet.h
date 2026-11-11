/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTEffects.h"
#include <array>

namespace JSC { namespace AOT { namespace Typed {

enum class KindOfEffectsRow : uint8_t { None, Operation, RuntimeOperation, Stub, Builtin, Callee, Summary, RestartMark, RowOfLoop };
enum class PartOfEffects : uint8_t { None, RunsOtherCode, Throws, Writes, Collects, ThrowsByRule, RangeOfResult, WritesObservableState, KindOfResult, ContentsOfResult, TestsOfGuard, ShapesKnown, ShapesOfField, MiddleShapesOfField, FreshnessOfResult };

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

enum class SecondLaneInEffectsNet : uint8_t { Structure, Prototype, NamedProperties, BytesOfView, Environment, Collection, InternalFields, RegExp };
inline constexpr std::array<AbstractHeapSet, 8> heapsOfSecondDigestInEffectsNet {
    AbstractHeapSet { AbstractHeap::ShapeWords, AbstractHeap::NamedProperties, AbstractHeap::PrototypeLinks, AbstractHeap::ArrayStorage },
    AbstractHeapSet { AbstractHeap::PrototypeLinks },
    AbstractHeapSet { AbstractHeap::NamedProperties },
    AbstractHeapSet { AbstractHeap::TypedArrayElements },
    AbstractHeapSet { AbstractHeap::EnvironmentFields },
    AbstractHeapSet { AbstractHeap::CollectionStorage, AbstractHeap::CollectionEntries },
    AbstractHeapSet { AbstractHeap::InternalFields, AbstractHeap::PromiseState },
    AbstractHeapSet { AbstractHeap::RegExpState },
};
constexpr uint8_t secondLanesNotWrittenInEffectsNet(const Effects& effects)
{
    uint8_t lanes = 0;
    for (unsigned lane = 0; lane < heapsOfSecondDigestInEffectsNet.size(); ++lane) {
        if (!effects.writes.containsAny(heapsOfSecondDigestInEffectsNet[lane]))
            lanes |= static_cast<uint8_t>(1u << lane);
    }
    return lanes;
}
inline constexpr uint64_t asksForSecondDigestInEffectsNet = 1ull << 62;
inline constexpr uint64_t asksForRestOfLastDigestInEffectsNet = 1ull << 60;
inline constexpr uint32_t speaksOfSecondDigestInEffectsNet = 1u << 15;
inline constexpr uint32_t speaksOfValueOfVariableInEffectsNet = 1u << 14;

enum class CountInEffectsNet : uint8_t {
    MemberGuardsEnded, MemberGuardsArmedOrBegun, WatchOverBuiltinsEnded, WordsOfStaticsWritten, VariablesOfModulesStored, StaticFieldsStored, VariablesOfScopesInStaticsStored
};
inline constexpr std::array<AbstractHeapSet, 7> heapsOfCountsInEffectsNet {
    AbstractHeapSet { AbstractHeap::MemberGuards },
    AbstractHeapSet { AbstractHeap::MemberGuards },
    AbstractHeapSet { AbstractHeap::NamedProperties },
    AbstractHeapSet { AbstractHeap::ModuleVariables, AbstractHeap::EnvironmentFields, AbstractHeap::NamedProperties },
    AbstractHeapSet { AbstractHeap::ModuleVariables },
    AbstractHeapSet { AbstractHeap::NamedProperties },
    AbstractHeapSet { AbstractHeap::EnvironmentFields },
};
inline constexpr unsigned bitsOfCountInEffectsNet = 64 / heapsOfCountsInEffectsNet.size();
constexpr uint64_t countInWordOfEffectsNet(uint64_t word, unsigned count) { return word >> count * bitsOfCountInEffectsNet & ((1ull << bitsOfCountInEffectsNet) - 1); }
constexpr uint64_t wordOfCountInEffectsNet(uint64_t writes, unsigned count) { return (writes * 0x9e3779b97f4a7c15ull >> (64 - bitsOfCountInEffectsNet)) << count * bitsOfCountInEffectsNet; }
inline constexpr uint64_t countsStoreInEffectsNet = 1ull << 61;
constexpr uint8_t stableCountsInEffectsNet(const Effects& effects)
{
    uint8_t counts = 0;
    for (unsigned count = 0; count < heapsOfCountsInEffectsNet.size(); ++count) {
        if (!effects.writes.containsAny(heapsOfCountsInEffectsNet[count]))
            counts |= static_cast<uint8_t>(1u << count);
    }
    return counts;
}
inline constexpr uint64_t asksForCountsInEffectsNet = 1ull << 63;
inline constexpr uint32_t noOperandInEffectsNet = UINT16_MAX;
inline constexpr uint32_t variableInEffectsNet = 1u << 15;

inline constexpr unsigned firstOperationRowInEffectsNet = 0;
inline constexpr unsigned firstRuntimeOperationRowInEffectsNet = 2048;
inline constexpr unsigned firstStubRowInEffectsNet = 6144;
inline constexpr unsigned firstBuiltinRowInEffectsNet = 8192;
inline constexpr unsigned numberOfRowsInEffectsNet = 24576;
inline constexpr unsigned noRowInEffectsNet = UINT16_MAX;
inline constexpr unsigned bytesOfRowInEffectsNet = 4;
constexpr unsigned byteInEffectsNet(unsigned row, PartOfEffects part) { return row * bytesOfRowInEffectsNet + static_cast<unsigned>(part) - 1; }

struct UnderstatedEffects {
    KindOfEffectsRow kind { KindOfEffectsRow::None };
    PartOfEffects part { PartOfEffects::None };
    AbstractHeap heap { };
    uint16_t row { 0 };

    static constexpr UnderstatedEffects decode(uint32_t word)
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
        case PartOfEffects::RangeOfResult:
        case PartOfEffects::KindOfResult:
        case PartOfEffects::ContentsOfResult:
        case PartOfEffects::TestsOfGuard:
        case PartOfEffects::ShapesKnown:
        case PartOfEffects::ShapesOfField:
        case PartOfEffects::MiddleShapesOfField:
        case PartOfEffects::FreshnessOfResult:
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
        case PartOfEffects::WritesObservableState:
            effects.writes = effects.writes.restrictedTo(unobservableWriteHeaps);
            return;
        case PartOfEffects::Collects:
            effects.mayCollect = false;
            return;
        case PartOfEffects::ThrowsByRule:
            effects.mayThrow &= ThrowEffect::onlyForLackOfMemory();
            return;
        }
    }
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
