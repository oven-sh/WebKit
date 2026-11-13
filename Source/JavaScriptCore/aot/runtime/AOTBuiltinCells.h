/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTBuiltinTableRealm.h"
#include "AOTVector.h"
#include "AOTWatchpoints.h"
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

inline constexpr uint32_t limitOfNumbersOfBuiltinCells = 1 << 15;

class BuiltinCells final : public WritesToBuiltinMembers {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(BuiltinCells);
    WTF_MAKE_NONCOPYABLE(BuiltinCells);
public:
    explicit BuiltinCells(State&);

    static constexpr size_t stackKeptFromProgram = (ASAN_ENABLED ? 3 : 1) * 256 * KB;

    JSCell* cell(uint32_t number);
    JSCell* cellReadWithoutTest(uint32_t number);
    JSCell* findObject(BuiltinID);
    JSCell* findGetter(BuiltinID);
    std::optional<BuiltinCellKey> keyOfCellHolding(JSCell*) const;
    JSCell* readMember(JSGlobalObject*, JSObject* holder, BuiltinID member);
    JSCell* findFunctionAsBuilt(BuiltinBoxedStub);
    EncodedJSValue* addressOfValue(BuiltinCellKey);
    JSValue fillValue(JSGlobalObject*, BuiltinCellKey);
    JSValue fillWatchedValue(JSGlobalObject*, BuiltinCellKey);
    ALWAYS_INLINE JSValue readValueInPlace(uint32_t encodedKey) const
    {
        for (const ValueReadInPlace& read : m_valuesReadInPlace) {
            if (read.encodedKey == encodedKey)
                return read.holder->structureID() == read.structure ? read.holder->getDirect(read.offset) : JSValue();
        }
        return { };
    }
    void forgetValuesNamed(UniquedStringImpl* key);
    void willWrite(BuiltinID member, UniquedStringImpl* key) final;
    void willAddNameTo(BuiltinID holder) final;
    void didMakeMember(BuiltinID member, JSObject*);
    void cannotHearOfWritesTo(BuiltinID holder);
    void hearsOfWritesTo(BuiltinID holder);
    void noteWriteAboveInstances(BuiltinID holder);
    void takeAsWritten(BuiltinID member);
    void takeAsHolderWithAddedNames(BuiltinID holder);
    void fillCellsUnderModules();
    void didLoadModule(RuntimeModuleID, JSObject* exports);
    void didLoadNativeModule(RuntimeModuleID, JSObject* exports);
    void didCreateMember(JSObject* holder, UniquedStringImpl* key, JSObject* member);
    JSObject* takePreallocatedMember(JSObject* holder, UniquedStringImpl* key);
    JSObject* memberMadeAhead(JSObject* holder, UniquedStringImpl* key) const;

    bool didLock(BuiltinID, JSObject*);
    bool hasLocked(BuiltinID id) const { return m_lockedBuiltins.get(id.index()); }
    BuiltinID builtinOfLockedObject(JSObject*) const;
    void awaitConstructor(JSObject* prototype, BuiltinID constructor) { m_awaitedConstructors.append({ prototype, constructor }); }
    BuiltinID takeAwaitedConstructor(JSObject* prototype)
    {
        size_t index = m_awaitedConstructors.findIf([&](const AwaitedConstructor& awaited) { return awaited.prototype == prototype; });
        if (index == notFound)
            return { };
        BuiltinID constructor = m_awaitedConstructors[index].constructor;
        m_awaitedConstructors.removeAt(index);
        return constructor;
    }

    const BitVector& builtinsToLock() const { return m_builtinsToLock; }
    const BitVector& holdersThatTellOfWrites() const { return m_holdersThatTellOfWrites; }
    void setBuiltinsToLock(BitVector&& builtins, BitVector&& holdersThatTellOfWrites)
    {
        m_builtinsToLock = WTF::move(builtins);
        m_holdersThatTellOfWrites = WTF::move(holdersThatTellOfWrites);
        m_lockedBuiltins.ensureSize(m_builtinsToLock.size());
        if (m_lockedObjects.isEmpty()) {
            size_t capacity = m_builtinsToLock.bitCount();
            m_lockedObjects.reserveInitialCapacity(capacity);
            m_builtinsOfLockedObjects.reserveInitialCapacity(capacity);
        }
    }

    using ObjectsByBuiltin = UncheckedKeyHashMap<uint32_t, JSObject*, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;
    const ObjectsByBuiltin& lockedObjects() const { return m_lockedObjects; }

private:
    std::optional<uint32_t> numberOf(BuiltinCellKey) const;
    void takeFunctionAsWritten(BuiltinID member);
    JSObject* findKnownObject(BuiltinID) const;
    std::expected<JSCell*, BuiltinLookupFailure> find(BuiltinCellKey);
    std::expected<JSCell*, BuiltinLookupFailure> findInKnownHolder(BuiltinCellKey, JSObject* holder);
    void fillAndKeepAlive(uint32_t number, JSCell*);
    bool awaitCreation(uint32_t number);
    void loadModuleOf(BuiltinID);

    State& m_state;
    Instance& m_instance;
    const ProgramTables& m_tables;
    JSCell** m_cells;
    uint32_t m_numberOfFirstValue;
    std::span<EncodedJSValue> m_values;
    Vector<std::unique_ptr<PropertyValueWatchpoint>> m_watchesOfValues;
    struct ValueReadInPlace {
        uint32_t encodedKey { 0 };
        JSObject* holder { nullptr };
        StructureID structure;
        PropertyOffset offset { invalidOffset };
    };
    Vector<ValueReadInPlace, 2> m_valuesReadInPlace;
    BitVector m_isUnavailable;
    struct AwaitedMember {
        JSObject* holder;
        Identifier key;
        uint32_t number;
    };
    Vector<AwaitedMember, 4> m_awaitedMembers;
    struct MemberMadeAhead {
        JSObject* holder;
        Identifier key;
        JSObject* member;
    };
    Vector<MemberMadeAhead> m_membersMadeAhead;
    std::array<std::pair<BuiltinID, uint32_t>, 16> m_recentObjects { };
    struct AwaitedConstructor {
        JSObject* prototype;
        BuiltinID constructor;
    };
    Vector<AwaitedConstructor> m_awaitedConstructors;
    struct CellAwaitingModule {
        uint32_t root;
        uint32_t number;
    };
    Vector<CellAwaitingModule> m_cellsAwaitingModules;
    ObjectsByBuiltin m_lockedObjects;
    BitVector m_lockedBuiltins;
    UncheckedKeyHashMap<JSObject*, uint32_t> m_builtinsOfLockedObjects;
    BitVector m_builtinsToLock;
    BitVector m_holdersThatTellOfWrites;
    BitVector m_writtenMembers;
    std::span<uint8_t> m_classesWrittenAbove;
    std::span<uint8_t> m_membersWritten;
    enum class NamesOnHolder : uint8_t { AreAsBuilt, AreNotHeardOf, WereAdded };
    std::span<NamesOnHolder> m_namesOnHolders;
    BitVector m_holdersWhoseWriteIsNoted;
    ObjectsByBuiltin m_membersMadeBehindAnnouncedWrite;
};

JS_EXPORT_PRIVATE void hostDidReplaceProperty(JSGlobalObject*, UniquedStringImpl* key);
JS_EXPORT_PRIVATE void hostDidLoadBuiltinModule(JSGlobalObject*, uint32_t module, JSValue exports);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
