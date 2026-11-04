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
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <compare>
#include <span>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/StringView.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT {

class BuiltinTable;
class ParallelLoops;

namespace Typed {

Vector<char16_t> decodeProgramString(std::span<const uint8_t>);
// The empty string and a string of one Latin-1 character: the VM has their cells from its start to its end.
bool hasCellOfVM(std::span<const uint8_t>);
Vector<uint8_t> encodeProgramString(std::span<const char16_t>);
Vector<uint8_t> encodeProgramString(StringView);
bool isCanonicalProgramString(std::span<const uint8_t>);
std::strong_ordering compareProgramStrings(std::span<const uint8_t>, std::span<const uint8_t>);
// In UTF-8 a larger byte begins a character outside Latin-1: the engine holds such a text in 16-bit characters.
inline bool isWideProgramString(std::span<const uint8_t> bytes) { return std::ranges::any_of(bytes, [](uint8_t byte) { return byte > 0xc3; }); }
StringView viewOfProgramString(std::span<const uint8_t>, Vector<char16_t>& bufferForNonASCII);

struct StringAsRead {
    std::span<const uint8_t> bytes;
    NameClass nameClass { NameClass::Ordinary };
    BuiltinName builtinName;
};

class StringTable {
    WTF_MAKE_NONCOPYABLE(StringTable);
public:
    StringTable() = default;

    // It checks nothing of the strings as read: AOTProgramVerifier.h does, where checks run.
    void initialize(const BuiltinTable&, const ParallelLoops&, Vector<StringAsRead>&&);

    size_t size() const { return m_strings.size(); }
    std::span<const uint8_t> operator[](StringID id) const { return m_strings[id.index()]; }
    StringID emptyString() const { return m_emptyString; }
    StringID comma() const { return m_comma; }
    // What ToString makes of the value. Every program holds these, whatever it spells itself.
    StringID textOfUndefined() const { return m_textOfUndefined; }
    StringID textOfNull() const { return m_textOfNull; }
    StringID textOf(bool value) const { return value ? m_textOfTrue : m_textOfFalse; }
    NameClass nameClass(StringID id) const { return m_nameClasses[id.index()]; }
    BuiltinName builtinName(StringID) const;
    StringID stringOf(BuiltinName textName) const { return m_builtinNameStrings[textName.index()]; }

    bool isAllASCII(StringID) const;
    StringView viewOf(StringID, Vector<char16_t>& bufferForNonASCII) const;
    String toWTFString(StringID) const;

    StringID find(std::span<const uint8_t> canonicalProgramString) const;
    // The string of the one code unit at the position. Null: the position lies outside the string.
    // Every code unit of a string of the table is a string of the table. So whether Compile finds it does not depend on what else the program spells.
    StringID codeUnitAt(StringID, uint64_t position) const;
    // A new string moves the lists of the table: no other thread may read the table meanwhile.
    StringID intern(std::span<const uint8_t>);
    StringID intern(StringView);
    // TEXTS THAT SHARE THEIR BYTES: each is the front of a longer one. keep() copies the bytes once, and they stay where they are as long as the table lives.
    // internKept() is intern() of a canonical program string that lies among them, and copies nothing. What it has done for a text it does not do again for a longer one that begins at the same address:
    // handed over from the shortest to the longest, all of them together cost what the longest holds. In any other order the answers are the same.
    struct FrontOfKeptTexts {
        uint64_t hashOfWords { 0 };
        size_t numberOfWords { 0 };
        size_t numberOfBytesLookedAt { 0 };
    };
    using FrontsOfKeptTexts = HashMap<const uint8_t*, FrontOfKeptTexts>;
    std::span<const uint8_t> keep(std::span<const uint8_t>);
    StringID internKept(std::span<const uint8_t>, FrontsOfKeptTexts&);
    // From here on the strings are those that somebody has interned. Only these share bytes.
    size_t numberOfStringsAsRead() const { return m_numberOfStringsAsRead; }

private:
    struct Shard {
        Vector<StringID> slots;
        uint32_t count { 0 };
    };
    static constexpr unsigned numberOfShards = 16;

    static uint64_t hashOf(std::span<const uint8_t>);
    static uint64_t hashGoingOnFrom(FrontOfKeptTexts&, std::span<const uint8_t>);
    uint64_t hashOf(StringID) const;
    static unsigned shardIndexOf(uint64_t hash) { return static_cast<unsigned>(hash % numberOfShards); }
    static size_t firstSlotOf(const Shard& shard, uint64_t hash) { return (hash / numberOfShards) & (shard.slots.size() - 1); }
    static size_t nextSlotOf(const Shard& shard, size_t slot) { return (slot + 1) & (shard.slots.size() - 1); }
    StringID find(const Shard&, uint64_t hash, std::span<const uint8_t>) const;
    static void insert(Shard&, uint64_t hash, StringID);
    void reserve(Shard&, size_t count) const;
    StringID append(std::span<const uint8_t>, uint64_t hash);
    StringID append(std::span<const uint8_t>, uint64_t hash, BuiltinName);
    void recordBuiltinName(StringID);
    void appendBuiltinNames();
    // One bit for each code unit.
    using CodeUnitSet = std::array<uint64_t, 1024>;
    static void addCodeUnitsOf(std::span<const uint8_t>, CodeUnitSet&);
    void internCodeUnitsIn(const CodeUnitSet&);
    // All of the text, of which the first `numberOfBytesLookedAt` hold no code unit that the table lacks.
    void internCodeUnitsOf(std::span<const uint8_t>, size_t numberOfBytesLookedAt = 0);
    StringID internCodeUnit(char16_t);

    const BuiltinTable* m_builtins { nullptr };
    Vector<std::span<const uint8_t>> m_strings;
    Vector<NameClass> m_nameClasses;
    Vector<BuiltinName> m_builtinNames;
    Vector<StringID> m_builtinNameStrings;
    Vector<Vector<uint8_t>> m_ownStrings;
    size_t m_numberOfStringsAsRead { 0 };
    // [StringID - m_numberOfStringsAsRead] A shard that grows asks for the hash of each of its strings.
    Vector<uint64_t> m_hashesOfInternedStrings;
    CodeUnitSet m_internedCodeUnits { };
    std::array<Shard, numberOfShards> m_shards;
    StringID m_emptyString;
    StringID m_comma;
    StringID m_textOfUndefined;
    StringID m_textOfNull;
    StringID m_textOfTrue;
    StringID m_textOfFalse;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
