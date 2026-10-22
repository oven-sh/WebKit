/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTBuiltinTableRealm.h"
#include "Identifier.h"
#include "JSCJSValue.h"
#include "JSExportMacros.h"
#include <expected>
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/FastMalloc.h>
#include <wtf/Forward.h>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

namespace JSC {

class JSCell;
class JSGlobalObject;
class JSObject;

namespace AOT {

class BuiltinCellKey;
enum class BuiltinLookupFailure : uint8_t;
struct Instance;
struct ProgramTables;
struct State;

class BuiltinCells final : public WritesToBuiltinMembers {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(BuiltinCells);
    WTF_MAKE_NONCOPYABLE(BuiltinCells);
public:
    explicit BuiltinCells(State&);

    // Compiled code asks for a cell wherever it stands, also in the handler of a stack overflow, and has no place for an exception there.
    static constexpr size_t stackKeptFromProgram = (ASAN_ENABLED ? 3 : 1) * 256 * KB;

    JSCell* cell(uint32_t number);
    JSCell* findObject(BuiltinID);
    // The key of a cell that is filled with the object. A cell holds what the realm was created with, whatever the program has put in its place since. For a validating build: it looks at every cell.
    std::optional<BuiltinCellKey> keyOfCellHolding(JSCell*) const;
    JSCell* readMember(JSGlobalObject*, JSObject* holder, BuiltinID member);
    // What the host makes at the first read of a member, and only a write replaces.
    EncodedJSValue* addressOfValue(BuiltinCellKey);
    JSValue fillValue(JSGlobalObject*, BuiltinCellKey);
    void forgetValuesNamed(UniquedStringImpl* key);
    // A cell holds what the realm was created with, and is filled at its first use: nothing is looked up before the program runs.
    void willWrite(BuiltinID member, UniquedStringImpl* key) final;
    // Not every write that is announced is made. The holder makes a member only where nobody has put anything, so what it makes is what the realm was created with.
    void didMakeMember(BuiltinID member, JSObject*);
    // The holder is known and nobody but the realm has reached it yet.
    void cannotHearOfWritesTo(BuiltinID holder);
    // The exports of a built-in module tell of nothing.
    void fillCellsUnderModules();
    void didLoadModule(RuntimeModuleID, JSObject* exports);
    void didCreateMember(JSObject* holder, UniquedStringImpl* key, JSObject* member);
    void fillCellsOfEngine();

    // The program may replace a step of the path to a locked object, such as the global `Response`, so a cell is looked for in the object that was locked when the realm created it.
    bool didLock(BuiltinID, JSObject*);
    // A validating build asks each of these behind every collection, and to lock an object allocates. So an object is listed once it is locked.
    void didLockEveryPropertyOf(JSObject* object) { m_objectsWithEveryPropertyLocked.append(object); }
    bool hasLocked(BuiltinID id) const { return m_lockedObjects.contains(id.index()); }
    BuiltinID builtinOfLockedObject(JSObject*) const;
    std::span<JSObject* const> objectsWithEveryPropertyLocked() const { return m_objectsWithEveryPropertyLocked.span(); }

    // The built-ins that are locked once the realm has created them, and the holders on the paths to them.
    const BitVector& builtinsToLock() const { return m_builtinsToLock; }
    // Among them, the holders of members that the program may replace.
    const BitVector& holdersThatTellOfWrites() const { return m_holdersThatTellOfWrites; }
    void setBuiltinsToLock(BitVector&& builtins, BitVector&& holdersThatTellOfWrites)
    {
        m_builtinsToLock = WTF::move(builtins);
        m_holdersThatTellOfWrites = WTF::move(holdersThatTellOfWrites);
    }

    using ObjectsByBuiltin = UncheckedKeyHashMap<uint32_t, JSObject*, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;
    // Also the objects of which only the listed properties are locked.
    const ObjectsByBuiltin& lockedObjects() const { return m_lockedObjects; }

private:
    std::optional<uint32_t> numberOf(BuiltinCellKey) const;
    JSObject* findKnownObject(BuiltinID) const;
    std::expected<JSCell*, BuiltinLookupFailure> find(BuiltinCellKey) const;
    void fillAndKeepAlive(uint32_t number, JSCell*);
    bool awaitCreation(uint32_t number);
    void loadModuleOf(BuiltinID);

    State& m_state;
    Instance& m_instance;
    const ProgramTables& m_tables;
    JSCell** m_cells;
    uint32_t m_numberOfFirstValue;
    std::span<EncodedJSValue> m_values;
    BitVector m_isUnavailable;
    struct AwaitedMember {
        JSObject* holder;
        Identifier key;
        uint32_t number;
    };
    Vector<AwaitedMember, 4> m_awaitedMembers;
    struct CellAwaitingModule {
        uint32_t root;
        uint32_t number;
    };
    Vector<CellAwaitingModule> m_cellsAwaitingModules;
    ObjectsByBuiltin m_lockedObjects;
    UncheckedKeyHashMap<JSObject*, uint32_t> m_builtinsOfLockedObjects;
    BitVector m_builtinsToLock;
    BitVector m_holdersThatTellOfWrites;
    BitVector m_writtenMembers;
    ObjectsByBuiltin m_membersMadeBehindAnnouncedWrite;
    Vector<JSObject*> m_objectsWithEveryPropertyLocked;
};

// The table leads from a module to its namespace object only.
class NamespacesOfBuiltinModules {
public:
    explicit NamespacesOfBuiltinModules(const BuiltinTable&);
    bool holdAbove(BuiltinID) const;

private:
    const BuiltinTable& m_table;
    BitVector m_namespaces;
};

// The table says of a property that the host makes at the first read that only a write replaces it.
JS_EXPORT_PRIVATE void hostDidReplaceProperty(JSGlobalObject*, UniquedStringImpl* key);
JS_EXPORT_PRIVATE void hostDidLoadBuiltinModule(JSGlobalObject*, uint32_t module, JSValue exports);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
