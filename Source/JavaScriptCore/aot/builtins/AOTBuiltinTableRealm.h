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

JS_EXPORT_PRIVATE std::expected<JSCell*, BuiltinLookupFailure> findInRealm(JSGlobalObject*, BuiltinCellKey);
// What the holder makes under the name at its first read, out of its static table, for a name that somebody wrote to before anybody had read it.
std::expected<JSCell*, BuiltinLookupFailure> makeMemberOfStaticTable(JSGlobalObject*, BuiltinCellKey, JSObject* holder);
JS_EXPORT_PRIVATE std::expected<JSCell*, BuiltinLookupFailure> findInHolder(JSGlobalObject*, BuiltinCellKey, JSObject* holder);
JS_EXPORT_PRIVATE std::expected<JSValue, BuiltinLookupFailure> findValueInHolder(JSGlobalObject*, JSObject* holder, const Identifier&);
JS_EXPORT_PRIVATE JSValue getByPath(JSGlobalObject*, BuiltinID);
// What the holder has under the key, if that is an object. Runs nothing and creates nothing: a property that the holder creates at its first read is not there yet.
JS_EXPORT_PRIVATE JSObject* findExistingObject(JSObject* holder, UniquedStringImpl* key);
// The member of the global object whose first read answers the constructor of this class.
BuiltinID findGlobalCreatedWith(JSGlobalObject*, const LazyClassStructure&);
// Visits the constructor of each class that the realm has created and that a member of the global object answers at its first read, whether or not anybody has read it.
void forEachCreatedClassOfGlobal(JSGlobalObject*, const ScopedLambda<void(BuiltinID, JSObject* constructor)>&);
// Visits the locked objects that exist and creates none: the end of a collection calls it on the collector's thread.
JS_EXPORT_PRIVATE void forEachLockedObject(JSGlobalObject*, const ScopedLambda<void(BuiltinID, JSObject*)>&);
JS_EXPORT_PRIVATE void forEachHiddenPrototype(JSGlobalObject*, const ScopedLambda<void(JSObject*)>&);
void forEachHiddenPrototype(JSGlobalObject*, const ScopedLambda<void(BuiltinClassID, JSObject*)>&);
// Visits the Structure that the realm gives a new instance, for each built-in class that has one and whose objects the realm has created. An array has one per kind of storage.
JS_EXPORT_PRIVATE void forEachInstanceStructure(JSGlobalObject*, const ScopedLambda<void(BuiltinClassID, Structure*)>&);
BuiltinClassID findClassWithInstanceStructure(JSGlobalObject*, Structure*);

// Hears of every write to a member of a built-in holder that nothing refuses, before it is made. What the engine or the host puts there when it makes a member is no write.
class WritesToBuiltinMembers {
public:
    virtual ~WritesToBuiltinMembers() = default;
    virtual void willWrite(BuiltinID member, UniquedStringImpl* key) = 0;
};

// For a holder that is not locked as a whole: the members that the table locks, the members that a program took for constants (their numbers, ascending), and what the table closes on the holder.
JS_EXPORT_PRIVATE Ref<ImmutablePropertyNames> immutablePropertyNamesOf(VM&, BuiltinID holder, std::span<const uint32_t> constantMembers = { }, WritesToBuiltinMembers* = nullptr);

} // namespace AOT
} // namespace JSC

#endif // ENABLE(AOT)
