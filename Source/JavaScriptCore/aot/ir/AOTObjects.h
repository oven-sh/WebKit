/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTTypeIDs.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>
#include <wtf/OptionSet.h>

namespace JSC { namespace AOT { namespace Typed {
struct ObjectRecord;
} } }

namespace WTF {

template<> struct VectorTraits<JSC::AOT::Typed::ObjectRecord> : VectorTraitsBase<false, void> {
    static constexpr bool needsInitialization = false;
    static constexpr bool canMoveWithMemcpy = true;
};

} // namespace WTF

namespace JSC { namespace AOT {

class BuiltinTable;
class ParallelLoops;

namespace Typed {

class Program;

enum class ObjectKind : uint8_t {
    Unknown,
    Builtin,
    ResultOfBuiltin,
    HostObject,
    EscapedFunctionsOfHolder,
    EscapedOfClass,
    EscapedWithoutShape,
    Closure,
    CoroutineResult,
    RestArray,
    FunctionPrototype,
    ConstructedObject,
    Namespace,
    ModuleBuiltin,
    Instance,
    Prototype,
    EscapedOfLayout,
};

enum class ObjectTrait : uint16_t {
    HasLayout = 1 << 0,
    HasClass = 1 << 1,
    HasNoShape = 1 << 2,
    IsClosure = 1 << 3,
    IsBuiltin = 1 << 4,
    IsOtherObject = 1 << 5,
    IsCallable = 1 << 6,
    IsMerged = 1 << 7,
    IsFoundByIdentity = 1 << 8,
    MayHaveLayoutBelow = 1 << 9,
    IsPlainObjectOfRows = 1 << 10,
};

inline constexpr OptionSet<ObjectTrait> traitsOfShape { ObjectTrait::HasLayout, ObjectTrait::HasClass, ObjectTrait::HasNoShape };
inline constexpr OptionSet<ObjectTrait> traitsOfIdentity { ObjectTrait::IsClosure, ObjectTrait::IsBuiltin, ObjectTrait::IsOtherObject };

enum class MakerOfShapelessObject : uint8_t { Host, OperationOrRow };

struct ObjectRecord {
    ObjectKind kind { ObjectKind::Unknown };
    OptionSet<ObjectTrait> traits;
    uint32_t payload { 0 };
    LayoutID layout;
    BuiltinClassID builtinClass;
    uint32_t ordinal { 0 };
    ModuleID module;

    bool has(ObjectTrait trait) const { return traits.contains(trait); }
};

inline constexpr uint32_t ordinalOfGetterFunctionOfAccessor = 2;
inline constexpr uint32_t ordinalOfSetterFunctionOfAccessor = 3;

inline bool isFunctionOfNativeAccessor(const ObjectRecord& record)
{
    return record.kind == ObjectKind::ResultOfBuiltin && record.ordinal >= ordinalOfGetterFunctionOfAccessor;
}

bool callOrReadOfBuiltinIsEvent(const BuiltinTable&, BuiltinID canonical);
bool rulesMeetBuiltinByName(const BuiltinTable&, BuiltinID canonical);

struct FunctionInOrderOfObjects {
    enum class Group : uint8_t { PlainFunctionOrClass, Arrow, MethodOrAccessor, EnteredByEngineAlone, WithoutBody };
    static constexpr unsigned numberOfGroups = static_cast<unsigned>(Group::WithoutBody) + 1;

    Group group { Group::PlainFunctionOrClass };
    bool isCoroutine { false };
    bool hasRestArray { false };
};

class ObjectTable {
    WTF_MAKE_NONCOPYABLE(ObjectTable);
public:
    JS_EXPORT_PRIVATE ObjectTable(const BuiltinTable&, std::span<const FunctionInOrderOfObjects>, const ParallelLoops* = nullptr);

    JS_EXPORT_PRIVATE void addObjectsOfProgram(const Program&, const BitVector& modulesNamedByModuleBuiltins, const ParallelLoops&);

    uint32_t size() const { return m_records.size(); }
    const ObjectRecord& at(ObjectID object) const { return m_records[object.index()]; }
    ObjectKind kindOf(ObjectID object) const { return at(object).kind; }
    LayoutID layoutOf(ObjectID object) const { return at(object).layout; }
    BuiltinClassID classOf(ObjectID object) const { return at(object).builtinClass; }
    OptionSet<ObjectTrait> traitsOf(ObjectID object) const { return at(object).traits; }

    static constexpr ObjectID unknown() { return ObjectID(0u); }
    JS_EXPORT_PRIVATE ObjectID objectOf(BuiltinID) const;
    JS_EXPORT_PRIVATE ObjectID objectOf(BuiltinID, ModuleID) const;
    template<size_t inlineCapacity>
    void appendObjectsOf(BuiltinID canonical, Vector<ObjectID, inlineCapacity>& objects) const
    {
        static_assert(ObjectKind::Builtin < ObjectKind::ModuleBuiltin);
        std::span<const ModuleBuiltin> all = m_moduleBuiltins.span();
        auto ofModules = std::ranges::equal_range(all, canonical, { }, &ModuleBuiltin::builtin);
        uint32_t first = firstOf(ObjectKind::ModuleBuiltin) + static_cast<uint32_t>(ofModules.begin() - all.begin());
        objects.reserveCapacity(objects.size() + 1 + ofModules.size());
        objects.append(objectOf(canonical));
        for (uint32_t index = 0; index < ofModules.size(); ++index)
            objects.append(ObjectID(first + index));
    }
    JS_EXPORT_PRIVATE ObjectID resultOf(BuiltinID, uint32_t ordinal) const;
    ObjectID hostObject(uint32_t indexOfRecord) const { return ObjectID(m_first[indexOf(ObjectKind::HostObject)] + indexOfRecord); }
    JS_EXPORT_PRIVATE ObjectID escapedFunctionsOf(BuiltinID holder) const;
    ObjectID escapedOf(BuiltinClassID builtinClass) const { return ObjectID(m_first[indexOf(ObjectKind::EscapedOfClass)] + builtinClass.index()); }
    ObjectID escapedWithoutShape(MakerOfShapelessObject maker) const { return ObjectID(m_first[indexOf(ObjectKind::EscapedWithoutShape)] + static_cast<uint32_t>(maker)); }
    ObjectID closureOf(FunctionID function) const { return ObjectID(m_first[indexOf(ObjectKind::Closure)] + m_placesByGroup[function.index()]); }
    ObjectID coroutineResultOf(FunctionID function) const { return ObjectID(m_first[indexOf(ObjectKind::CoroutineResult)] + m_placesWithCoroutinesFirst[function.index()]); }
    ObjectID restArrayOf(FunctionID function) const { return ObjectID(m_first[indexOf(ObjectKind::RestArray)] + m_placesWithRestArraysFirst[function.index()]); }
    ObjectID prototypeObjectOf(FunctionID function) const { return ObjectID(m_first[indexOf(ObjectKind::FunctionPrototype)] + m_placesByGroup[function.index()]); }
    ObjectID constructedObjectOf(FunctionID newTarget) const { return ObjectID(m_first[indexOf(ObjectKind::ConstructedObject)] + m_placesByGroup[newTarget.index()]); }
    ObjectID namespaceOf(ModuleID module) const { return ObjectID(m_first[indexOf(ObjectKind::Namespace)] + module.index()); }
    ObjectID instanceOf(LayoutID layout) const { return ObjectID(m_first[indexOf(ObjectKind::Instance)] + m_placesOfLayouts[layout.index()]); }
    ObjectID prototypeOf(LayoutID layout) const { return ObjectID(m_first[indexOf(ObjectKind::Prototype)] + m_placesOfLayouts[layout.index()]); }
    ObjectID escapedOf(LayoutID layout) const { return ObjectID(m_first[indexOf(ObjectKind::EscapedOfLayout)] + m_placesOfLayouts[layout.index()]); }

    FunctionID closureFunction(ObjectID object) const { return at(object).kind == ObjectKind::Closure ? FunctionID(at(object).payload) : FunctionID(); }
    BuiltinID builtinOf(ObjectID object) const { return at(object).kind == ObjectKind::Builtin || at(object).kind == ObjectKind::ModuleBuiltin ? BuiltinID(at(object).payload) : BuiltinID(); }
    JS_EXPORT_PRIVATE ObjectID mergedFormOf(ObjectID) const;

    uint32_t firstOf(ObjectKind kind) const { return m_first[indexOf(kind)]; }
    uint32_t endOf(ObjectKind kind) const { return m_first[indexOf(kind) + 1]; }

private:
    static constexpr unsigned numberOfKinds = static_cast<unsigned>(ObjectKind::EscapedOfLayout) + 1;
    static constexpr unsigned indexOf(ObjectKind kind) { return static_cast<unsigned>(kind); }
    std::optional<MakerOfShapelessObject> makerOfShapelessObjectThatMerges(ObjectID) const;

    struct ModuleBuiltin {
        BuiltinID builtin;
        ModuleID module;

        friend constexpr auto operator<=>(const ModuleBuiltin&, const ModuleBuiltin&) = default;
    };

    const BuiltinTable& m_builtins;
    std::array<uint32_t, numberOfKinds + 1> m_first { };
    Vector<ObjectRecord> m_records;
    Vector<uint32_t> m_placesByGroup;
    Vector<uint32_t> m_placesWithCoroutinesFirst;
    Vector<uint32_t> m_placesWithRestArraysFirst;
    Vector<uint32_t> m_placesOfLayouts;
    Vector<ModuleBuiltin> m_moduleBuiltins;
    BitVector m_builtinFunctionsThatStayApart;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
