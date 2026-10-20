/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "JSExportMacros.h"
#include <expected>
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/FastMalloc.h>
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

class BuiltinCells {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(BuiltinCells);
    WTF_MAKE_NONCOPYABLE(BuiltinCells);
public:
    explicit BuiltinCells(State&);

    // Compiled code asks for a cell wherever it stands, also in the handler of a stack overflow, and has no place for an exception there.
    // A host may run JavaScript to create the object: it sets up a queue or loads a module. So the program never gets the last part of the stack, and the creation may use it.
    static constexpr size_t stackKeptFromProgram = (ASAN_ENABLED ? 3 : 1) * 256 * KB;

    JSCell* cell(uint32_t number);
    JSCell* findObject(BuiltinID);
    JSCell* readMember(JSGlobalObject*, JSObject* holder, BuiltinID member);
    // A cell holds what the realm was created with. Where the program may put something else, the cell is filled before the program runs.
    void fillEagerCells();
    void fillCellsOfEngine();

    // The program may replace a step of the path to a locked object, such as the global `Response`, so a cell is looked for in the object that was locked when the realm created it.
    enum class LockedProperties : bool { All, Listed };
    // Whether the object is new to the cells.
    bool didLock(BuiltinID, JSObject*, LockedProperties);
    bool hasLocked(BuiltinID id) const { return m_lockedObjects.contains(id.index()); }
    BuiltinID builtinOfLockedObject(JSObject*) const;
    std::span<JSObject* const> objectsWithEveryPropertyLocked() const { return m_objectsWithEveryPropertyLocked.span(); }

    // The built-ins that are locked once the realm has created them, and the holders on the paths to them.
    const BitVector& builtinsToLock() const { return m_builtinsToLock; }
    void setBuiltinsToLock(BitVector&& builtins) { m_builtinsToLock = WTF::move(builtins); }

    using ObjectsByBuiltin = UncheckedKeyHashMap<uint32_t, JSObject*, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;

private:
    std::optional<uint32_t> numberOf(BuiltinCellKey) const;
    JSObject* findKnownObject(BuiltinID) const;
    std::expected<JSCell*, BuiltinLookupFailure> find(BuiltinCellKey) const;
    void fillAndKeepAlive(uint32_t number, JSCell*);
    void loadModuleOf(BuiltinID);

    State& m_state;
    Instance& m_instance;
    const ProgramTables& m_tables;
    JSCell** m_cells;
    BitVector m_isUnavailable;
    ObjectsByBuiltin m_lockedObjects;
    UncheckedKeyHashMap<JSObject*, uint32_t> m_builtinsOfLockedObjects;
    BitVector m_builtinsToLock;
    Vector<JSObject*> m_objectsWithEveryPropertyLocked;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
