/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

#include "B3Value.h"

namespace JSC { namespace B3 {

struct RelocationSymbol {
    uint32_t space;
    uint32_t index;

    friend bool operator==(const RelocationSymbol&, const RelocationSymbol&) = default;
};

static constexpr uint32_t runtimeFunctionSpace = 0;

enum class RuntimeFunction : uint32_t {
    FmodDouble,
    CeilDouble,
    CeilFloat,
    FloorDouble,
    FloorFloat,
    TruncDouble,
    TruncFloat,
    MemoryCopy,
    MemoryFill,
};

static constexpr uint32_t numberOfRuntimeFunctions = 9;
static_assert(static_cast<uint32_t>(RuntimeFunction::MemoryFill) + 1 == numberOfRuntimeFunctions);

class JS_EXPORT_PRIVATE RelocatableValue final : public Value {
public:
    static bool accepts(Kind kind) { return kind == Relocatable; }

    ~RelocatableValue() final;

    RelocationSymbol symbol() const { return m_symbol; }
    int64_t addend() const { return m_addend; }

    B3_SPECIALIZE_VALUE_FOR_NO_CHILDREN

private:
    void dumpMeta(CommaPrinter&, PrintStream&) const final;

    friend class Procedure;
    friend class Value;

    static Opcode opcodeFromConstructor(Origin, RelocationSymbol, int64_t = 0) { return Relocatable; }
    RelocatableValue(Origin origin, RelocationSymbol symbol, int64_t addend = 0)
        : Value(CheckedOpcode, Relocatable, pointerType(), Zero, origin)
        , m_symbol(symbol)
        , m_addend(addend)
    {
    }

    RelocationSymbol m_symbol;
    int64_t m_addend;
};

JS_EXPORT_PRIVATE Value* addressOf(Procedure&, RuntimeFunction, Origin = Origin());

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
