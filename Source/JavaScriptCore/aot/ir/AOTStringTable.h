/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTID.h"
#include "AOTNameClass.h"
#include "AOTSchemaEnums.h"
#include <array>
#include <compare>
#include <span>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>
#include <wtf/text/StringView.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT {

class BuiltinTable;
class ParallelLoops;

namespace Typed {

class StringInterningPhase;

Vector<char16_t> decodeProgramString(std::span<const uint8_t>);
Vector<uint8_t> encodeProgramString(std::span<const char16_t>);
Vector<uint8_t> encodeProgramString(StringView);
bool isCanonicalProgramString(std::span<const uint8_t>);
// The order of the UTF-16 code units.
std::strong_ordering compareProgramStrings(std::span<const uint8_t>, std::span<const uint8_t>);

struct StringAsRead {
    std::span<const uint8_t> bytes;
    NameClass nameClass { NameClass::Ordinary };
    BuiltinName builtinName;
};

class StringTable {
    WTF_MAKE_NONCOPYABLE(StringTable);
public:
    enum class Error : uint8_t { None, DuplicateString, StringNotCanonical, NameClassMismatch };
    struct InitializationResult {
        Error error { Error::None };
        StringID string;
    };

    static constexpr uint32_t provisionalBit = 1u << 31;
    static constexpr bool isProvisional(StringID id) { return id && id.index() & provisionalBit; }

    StringTable() = default;

    InitializationResult initialize(const BuiltinTable&, const ParallelLoops&, Vector<StringAsRead>&&);

    size_t size() const { return m_strings.size(); }
    bool hasOpenPhase() const { return m_openPhase; }
    std::span<const uint8_t> operator[](StringID id) const { return isProvisional(id) ? provisionalAt(id) : m_strings[id.index()]; }
    StringID emptyString() const { return m_emptyString; }
    NameClass nameClass(StringID id) const { return isProvisional(id) ? classifyName(provisionalAt(id)) : m_nameClasses[id.index()]; }
    BuiltinName builtinName(StringID) const;
    StringID stringOf(BuiltinName textName) const { return m_builtinNameStrings[textName.index()]; }

    bool isAllASCII(StringID) const;
    StringView viewOf(StringID, Vector<char16_t>& bufferForNonASCII) const;
    String toWTFString(StringID) const;

    StringID find(std::span<const uint8_t> canonicalProgramString) const;
    StringID intern(std::span<const uint8_t>);
    StringID intern(StringView);

private:
    friend class StringInterningPhase;

    struct Shard {
        Vector<StringID> slots;
        uint32_t count { 0 };
    };
    static constexpr unsigned numberOfShards = 16;

    static uint64_t hashOf(std::span<const uint8_t>);
    static unsigned shardIndexOf(uint64_t hash) { return static_cast<unsigned>(hash % numberOfShards); }
    static size_t firstSlotOf(const Shard& shard, uint64_t hash) { return (hash / numberOfShards) & (shard.slots.size() - 1); }
    static size_t nextSlotOf(const Shard& shard, size_t slot) { return (slot + 1) & (shard.slots.size() - 1); }
    StringID find(const Shard&, uint64_t hash, std::span<const uint8_t>) const;
    static void insert(Shard&, uint64_t hash, StringID);
    void reserve(Shard&, size_t count) const;
    StringID append(std::span<const uint8_t>, uint64_t hash);
    void recordBuiltinName(StringID);
    void appendBuiltinNames();
    std::span<const uint8_t> provisionalAt(StringID) const;

    const BuiltinTable* m_builtins { nullptr };
    Vector<std::span<const uint8_t>> m_strings;
    Vector<NameClass> m_nameClasses;
    Vector<BuiltinName> m_builtinNames;
    Vector<StringID> m_builtinNameStrings;
    Vector<Vector<uint8_t>> m_ownStrings;
    std::array<Shard, numberOfShards> m_shards;
    StringID m_emptyString;
    const StringInterningPhase* m_openPhase { nullptr };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
