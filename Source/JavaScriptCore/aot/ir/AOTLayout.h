/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTID.h"
#include "AOTSchemaEnums.h"
#include "AOTSchemaRecords.h"
#include "AOTVector.h"
#include <optional>
#include <wtf/BitVector.h>
#include <wtf/OptionSet.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class Program;
struct Value;

constexpr bool isEnvironmentKind(LayoutKind kind)
{
    switch (kind) {
    case LayoutKind::Environment:
    case LayoutKind::Closure:
    case LayoutKind::CoroutineFrame:
        return true;
    case LayoutKind::Record:
    case LayoutKind::Instance:
    case LayoutKind::Container:
    case LayoutKind::BuiltinResult:
    case LayoutKind::ModuleNamespace:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

inline constexpr uint32_t maxFieldsAlwaysInline = 14;

struct Layout : LayoutRecord {
    bool has(LayoutFlags flag) const { return flags.contains(flag); }
    bool isEnvironment() const { return isEnvironmentKind(kind); }
    bool isDerived() const { return has(LayoutFlags::IsDerived); }
    bool mayKeepFieldsOutOfLine() const { return fields.size() > maxFieldsAlwaysInline; }

    std::optional<uint32_t> findField(PropertyKeyID) const;
    void indexFields();

    uint32_t appendField(Field&&);

private:
    struct IndexedField {
        PropertyKeyID key;
        uint32_t field { 0 };
    };

    static uint32_t hashOf(PropertyKeyID key)
    {
        uint32_t hash = key.index() * 0x9e3779b1u;
        return hash ^ (hash >> 15);
    }
    void addToIndex(uint32_t field);

    Vector<IndexedField> m_fieldsByKey;
    uint64_t m_keysByHash { 0 };
    uint32_t m_indexedFieldCount { 0 };
};

inline std::optional<uint32_t> Layout::findField(PropertyKeyID key) const
{
    RELEASE_ASSERT(m_indexedFieldCount == fields.size());
    if (!key)
        return std::nullopt;
    uint32_t hash = hashOf(key);
    if (!(m_keysByHash & (1ull << (hash & 63))))
        return std::nullopt;
    if (m_fieldsByKey.isEmpty()) {
        for (uint32_t i = 0; i < fields.size(); ++i) {
            if (fields[i].key == key)
                return i;
        }
        return std::nullopt;
    }
    uint32_t mask = static_cast<uint32_t>(m_fieldsByKey.size()) - 1;
    for (uint32_t place = (hash >> 6) & mask;; place = (place + 1) & mask) {
        const IndexedField& indexed = m_fieldsByKey[place];
        if (indexed.key == key)
            return indexed.field;
        if (!indexed.key)
            return std::nullopt;
    }
}

struct FieldReference {
    LayoutID layout;
    uint32_t field { 0 };

    friend bool operator==(const FieldReference&, const FieldReference&) = default;
    friend auto operator<=>(const FieldReference&, const FieldReference&) = default;
};

enum class BaseChain : uint8_t { Proven, Predicted };

const Field& fieldAt(const Program&, FieldReference);
FieldReference declarationOf(const Program&, FieldReference);
const FieldFlow& flowOf(const Program&, FieldReference);
bool isNeverRedefined(const Program&, FieldReference);
bool isNeverDeleted(const Program&, FieldReference);
bool isNeverMovedOutByStore(const Program&, FieldReference);
bool slotTakesEveryValue(const Program&, const Field&);
bool staysInSlot(const Program&, FieldReference);
bool staysFilled(const Program&, FieldReference);
bool isFilledAtBirth(const Program&, FieldReference);
bool keepsStateOfBirth(const Program&, FieldReference);
bool isFilledWithValueAtBirth(const Program&, FieldReference);
bool hasFieldsFilledAtBirth(const Program&, LayoutID);
bool isNeverStored(const Program&, FieldReference);
std::optional<FieldReference> findField(const Program&, LayoutID, PropertyKeyID, BaseChain);
LayoutID expectedBaseOf(const Layout&);
LayoutID predictedHeritageOf(const Layout&);
BuiltinClassID builtinBaseClassOf(const Program&, LayoutID);
bool hasOnlyCompiledBases(const Program&, LayoutID);
bool isEvaluatedOnce(const Program&, const Layout&);
bool guardsAreNeverSet(const Program&, const Layout&);
bool prototypePropertyMayEndGuards(const Program&, FunctionID);
bool hasSingletonClosure(const Program&, FunctionID);
bool namesClosureOfTable(const Program&, const Value& closure);

Layout createDerivedLayout(const Program&, LayoutID base);
LayoutID declaredLayoutOf(const Program&, LayoutID);

Vector<LayoutSetID, 2> declaredFamiliesOf(const Program&, LayoutID);
Vector<Vector<LayoutID>> declaredViewsOf(const Program&, const BitVector& declaredLayouts);

void initializeLayouts(Program&, BuildContext&);
void initializeAddedLayout(Program&, LayoutID);
void appendInheritedFields(Program&, LayoutID);

void markUnallocatedLayouts(Program&, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
