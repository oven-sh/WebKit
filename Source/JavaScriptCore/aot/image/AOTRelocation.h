/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTVector.h"
#include "AssemblerPolicy.h"
#include "CCallHelpers.h"
#include "LinkBuffer.h"
#include "RelocationKind.h"

namespace JSC { namespace AOT {

class BuildContext;

enum class LinkFailure : uint8_t {
    ProcessAddressInCode,
    OutOfPCRelativeReach,
    OutOfMemory,
};

enum class SymbolSpace : uint8_t {
    Function,
    Stub,
    ArgumentThunk,
    OutlinedBody,
    Constant,
    JumpTable,
    SiteCache,
    RegExp,
    WebAssembly,
    FFI,
};
static constexpr unsigned numberOfSymbolSpaces = 10;
static_assert(static_cast<unsigned>(SymbolSpace::FFI) + 1 == numberOfSymbolSpaces);

static constexpr uint16_t primaryEntry = 0;

struct CodeSymbol {
    friend constexpr bool operator==(const CodeSymbol&, const CodeSymbol&) = default;

    SymbolSpace space;
    uint32_t index;
    uint16_t entry { primaryEntry };
};

struct Relocation {
    constexpr CodeSymbol codeSymbol() const { return { space, symbol, entry }; }

    friend constexpr bool operator==(const Relocation&, const Relocation&) = default;

    uint32_t offset;
    RelocationKind kind;
    SymbolSpace space;
    uint16_t entry;
    uint32_t symbol;
};
static_assert(sizeof(Relocation) == 12);

struct RelocatableCode {
    Vector<uint8_t> bytes;
    Vector<Relocation> relocations;
};

class JS_EXPORT_PRIVATE ImageLinkBuffer {
    WTF_MAKE_NONCOPYABLE(ImageLinkBuffer);
public:
    ImageLinkBuffer(CCallHelpers&, LinkBuffer::Profile);

    LinkBuffer& linkBuffer() { return m_linkBuffer; }
    bool didFailToAllocate() const { return m_linkBuffer.didFailToAllocate(); }

    void finalize(bool dumpsDisassembly, ASCIILiteral name);
    std::span<uint8_t> bytes();

private:
    CCallHelpers::Label m_end;
    Vector<uint32_t> m_storage;
    LinkBuffer m_linkBuffer;
    bool m_didFinalize { false };
};

class JS_EXPORT_PRIVATE RelocationRecorder {
    WTF_MAKE_NONCOPYABLE(RelocationRecorder);
public:
    RelocationRecorder(CCallHelpers&, const AssemblerPolicy&);

    void moveAddress(CodeSymbol, GPRReg dest, PCRelativeReach);
    void load64(CodeSymbol, GPRReg dest);
    uint32_t call(CodeSymbol);
    uint32_t jump(CodeSymbol);
    void moveInstanceAddress(CodeSymbol, GPRReg instance, GPRReg dest);
    void emitDistance32(CodeSymbol, int32_t addend);

    uint32_t numberOfRelocations() const { return m_pending.size(); }

    std::expected<RelocatableCode, LinkFailure> finalize(LinkBuffer::Profile, BuildContext*);
    std::expected<Vector<Relocation>, LinkFailure> link(ImageLinkBuffer&, BuildContext*);

private:
    struct PendingRelocation {
        CCallHelpers::Label site;
        RelocationKind kind;
        CodeSymbol symbol;
    };

    uint32_t append(CCallHelpers::Label, RelocationKind, CodeSymbol);

    CCallHelpers& m_jit;
    Vector<PendingRelocation, 4> m_pending;
};

// AddressFar is page relative on ARM64: code must be loaded at a multiple of 4 KB.
[[nodiscard]] ALWAYS_INLINE bool applyRelocation(std::span<uint8_t> code, size_t bodyStart, const Relocation& relocation, size_t target)
{
    return MacroAssembler::applyRelocation(relocation.kind, code, bodyStart + relocation.offset, target);
}

template<typename Resolver, typename Reporter>
uint64_t applyRelocations(std::span<uint8_t> code, size_t bodyStart, std::span<const Relocation> relocations, const Resolver& resolve, const Reporter& reportOutOfReach)
{
    uint64_t numberApplied = 0;
    for (const Relocation& relocation : relocations) {
        std::optional<size_t> target = resolve(relocation);
        if (!target)
            continue;
        if (!applyRelocation(code, bodyStart, relocation, *target)) {
            reportOutOfReach(relocation);
            continue;
        }
        ++numberApplied;
    }
    return numberApplied;
}

enum class RelocationSpace : uint32_t {
    B3RuntimeFunction = 0,
    WebAssemblyPointers,
    SiteCaches,
    Constants,
    JumpTables,
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
