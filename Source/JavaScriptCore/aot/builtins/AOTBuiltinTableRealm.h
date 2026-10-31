/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "ImmutablePropertyNames.h"
#include "JSCJSValue.h"
#include <expected>
#include <optional>
#include <span>
#include <wtf/Ref.h>
#include <wtf/ScopedLambda.h>

namespace JSC {

class Identifier;
class JSCell;
class JSGlobalObject;
class JSObject;
class LazyClassStructure;
class Structure;

namespace AOT {

class BuiltinCellKey {
public:
    enum class Kind : uint8_t {
        Object,
        Getter,
        Setter,
        Value,
    };

    static constexpr BuiltinCellKey object(BuiltinID id) { return BuiltinCellKey(Kind::Object, id); }
    static constexpr BuiltinCellKey getter(BuiltinID id) { return BuiltinCellKey(Kind::Getter, id); }
    static constexpr BuiltinCellKey setter(BuiltinID id) { return BuiltinCellKey(Kind::Setter, id); }
    static constexpr BuiltinCellKey value(BuiltinID id) { return BuiltinCellKey(Kind::Value, id); }

    constexpr uint32_t encoded() const { return static_cast<uint32_t>(m_kind) << 24 | m_builtin.index(); }
    JS_EXPORT_PRIVATE static std::optional<BuiltinCellKey> decode(uint32_t, const BuiltinTable&);

    constexpr Kind kind() const { return m_kind; }
    constexpr BuiltinID builtin() const { return m_builtin; }
    bool hasCell(const BuiltinTable&) const;

    friend constexpr bool operator==(BuiltinCellKey, BuiltinCellKey) = default;

private:
    constexpr BuiltinCellKey(Kind kind, BuiltinID builtin)
        : m_builtin(builtin)
        , m_kind(kind)
    {
    }

    BuiltinID m_builtin;
    Kind m_kind;
};

static_assert(sizeof(BuiltinCellKey) == 4);

enum class BuiltinLookupFailure : uint8_t {
    HolderIsMissing,
    IsMissing,
    IsAccessor,
    IsNotAccessor,
    HasNoGetter,
    HasNoSetter,
    LookupThrew,
    IsNotObject,
    IsNotSymbol,
    HasOtherCallee,
    IsPerModule,
};

// No program can name a member of such a group.
bool groupsFunctionsOfCompiledCode(BuiltinID);

JS_EXPORT_PRIVATE std::expected<JSCell*, BuiltinLookupFailure> findInRealm(JSGlobalObject*, BuiltinCellKey);
std::expected<JSCell*, BuiltinLookupFailure> makeMemberOfStaticTable(JSGlobalObject*, BuiltinCellKey, JSObject* holder);
JS_EXPORT_PRIVATE std::expected<JSCell*, BuiltinLookupFailure> findInHolder(JSGlobalObject*, BuiltinCellKey, JSObject* holder);
JS_EXPORT_PRIVATE std::expected<JSValue, BuiltinLookupFailure> findValueInHolder(JSGlobalObject*, JSObject* holder, const Identifier&);
JS_EXPORT_PRIVATE JSValue getByPath(JSGlobalObject*, BuiltinID);
JS_EXPORT_PRIVATE JSObject* findExistingObject(JSObject* holder, UniquedStringImpl* key);
// The object of a built-in, unless it or one of its holders is yet to be created. Allocates no cell.
JS_EXPORT_PRIVATE JSObject* findExistingBuiltin(JSGlobalObject*, BuiltinID);
BuiltinID findGlobalCreatedWith(JSGlobalObject*, const LazyClassStructure&);
// The constructor of a class that hangs on an object of the realm, as Temporal.Instant does.
BuiltinID findMemberOfNamespaceCreatedWith(JSGlobalObject*, const LazyClassStructure&);
void forEachCreatedClassOfGlobal(JSGlobalObject*, const ScopedLambda<void(BuiltinID, JSObject* constructor)>&);
JS_EXPORT_PRIVATE void forEachLockedObject(JSGlobalObject*, const ScopedLambda<void(BuiltinID, JSObject*)>&);
JS_EXPORT_PRIVATE void forEachHiddenPrototype(JSGlobalObject*, const ScopedLambda<void(JSObject*)>&);
void forEachHiddenPrototype(JSGlobalObject*, const ScopedLambda<void(BuiltinClassID, JSObject*)>&);
JS_EXPORT_PRIVATE void forEachInstanceStructure(JSGlobalObject*, const ScopedLambda<void(BuiltinClassID, Structure*)>&);
BuiltinClassID findClassWithInstanceStructure(JSGlobalObject*, Structure*);

class WritesToBuiltinMembers {
public:
    virtual ~WritesToBuiltinMembers() = default;
    virtual void willWrite(BuiltinID member, UniquedStringImpl* key) = 0;
};

// unlockedMembers: ProgramTables::unlockedBuiltinMembers(). openedNames: those of ProgramTables::openedNames() that are opened on the holder.
// A wholly locked object gets these names where a test run opens some of it: everything but what the two lists say stays closed.
JS_EXPORT_PRIVATE Ref<ImmutablePropertyNames> immutablePropertyNamesOf(VM&, BuiltinID holder, std::span<const uint32_t> constantMembers = { }, WritesToBuiltinMembers* = nullptr, std::span<const uint32_t> unlockedMembers = { }, bool reflectThrows = false, std::span<const Identifier> openedNames = { });

} // namespace AOT
} // namespace JSC

#endif // ENABLE(AOT)
