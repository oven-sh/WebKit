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
    Site,
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
    IsOfInstance = 1 << 10,
    IsPlainObjectOfRows = 1 << 11,
};

inline constexpr OptionSet<ObjectTrait> traitsOfShape { ObjectTrait::HasLayout, ObjectTrait::HasClass, ObjectTrait::HasNoShape };
inline constexpr OptionSet<ObjectTrait> traitsOfIdentity { ObjectTrait::IsClosure, ObjectTrait::IsBuiltin, ObjectTrait::IsOtherObject };

enum class SiteOrigin : uint8_t { Operation, DataNode, BuiltinResult };

enum class MakerOfShapelessObject : uint8_t { Host, OperationOrRow };

enum class SiteTrait : uint8_t {
    NamesPrototype = 1 << 0,
    IsProxyFromNamedConstructor = 1 << 1,
    IsDenseArrayLiteral = 1 << 2,
    IsPairLiteral = 1 << 3,
    IsAnswerOfCallWithOneArgumentAtMost = 1 << 4,
    IsTemplateObject = 1 << 5,
};

struct ObjectRecord {
    ObjectKind kind { ObjectKind::Site };
    SiteOrigin origin { SiteOrigin::Operation };
    OptionSet<ObjectTrait> traits;
    uint32_t payload { 0 };
    LayoutID layout;
    BuiltinClassID builtinClass;
    uint32_t ordinal { 0 };
    FunctionID owner;
    ModuleID module;
    ObjectID sharedForm;
    uint32_t instance { 0 };
    OptionSet<SiteTrait> siteTraits;

    bool has(ObjectTrait trait) const { return traits.contains(trait); }
    bool has(SiteTrait trait) const { return siteTraits.contains(trait); }
};

inline constexpr uint32_t ordinalOfGetterFunctionOfAccessor = 2;
inline constexpr uint32_t ordinalOfSetterFunctionOfAccessor = 3;

inline bool isFunctionOfNativeAccessor(const ObjectRecord& record)
{
    return record.kind == ObjectKind::ResultOfBuiltin && record.ordinal >= ordinalOfGetterFunctionOfAccessor;
}

struct ObjectSite {
    ValueID value;
    uint32_t ordinal { 0 };
    SiteOrigin origin { SiteOrigin::Operation };
    OptionSet<SiteTrait> traits;
    LayoutID layout;
    BuiltinClassID builtinClass;
    bool mayHaveLayoutBelow { false };
    DataID data;
};

struct ObjectOfInstance {
    uint32_t instance { 0 };
    ObjectID sharedForm;

    friend constexpr auto operator<=>(const ObjectOfInstance&, const ObjectOfInstance&) = default;
};

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
    JS_EXPORT_PRIVATE void reserveSites(std::span<const uint32_t> numberOfSitesOfFunctions, const ParallelLoops&);
    JS_EXPORT_PRIVATE void defineSites(FunctionID, std::span<const ObjectSite> sortedByValueAndOrdinal);
    JS_EXPORT_PRIVATE ObjectID addObjectsOfInstances(std::span<const ObjectOfInstance> withoutRepeats);

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
    JS_EXPORT_PRIVATE ObjectID siteObject(FunctionID, ValueID, uint32_t ordinal = 0) const;
    std::span<const ObjectRecord> sitesOf(FunctionID function) const LIFETIME_BOUND
    {
        return m_records.span().subspan(m_firstSiteOfFunction[function.index()], m_firstSiteOfFunction[function.index() + 1] - m_firstSiteOfFunction[function.index()]);
    }

    FunctionID closureFunction(ObjectID object) const { return at(object).kind == ObjectKind::Closure ? FunctionID(at(object).payload) : FunctionID(); }
    BuiltinID builtinOf(ObjectID object) const { return at(object).kind == ObjectKind::Builtin || at(object).kind == ObjectKind::ModuleBuiltin ? BuiltinID(at(object).payload) : BuiltinID(); }
    ObjectID sharedFormOf(ObjectID object) const { return at(object).sharedForm ? at(object).sharedForm : object; }
    JS_EXPORT_PRIVATE ObjectID mergedFormOf(ObjectID) const;
    ObjectID findObjectOfInstance(ObjectOfInstance wanted) const
    {
        for (uint32_t index = firstObjectOfInstances(); index < size(); ++index) {
            if (m_records[index].instance == wanted.instance && m_records[index].sharedForm == wanted.sharedForm)
                return ObjectID(index);
        }
        return { };
    }

    uint32_t firstOf(ObjectKind kind) const { return m_first[indexOf(kind)]; }
    uint32_t endOf(ObjectKind kind) const { return m_first[indexOf(kind) + 1]; }
    uint32_t firstObjectOfInstances() const { return m_first[numberOfKinds]; }

private:
    static constexpr unsigned numberOfKinds = static_cast<unsigned>(ObjectKind::Site) + 1;
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
    Vector<uint32_t> m_firstSiteOfFunction;
    Vector<ModuleBuiltin> m_moduleBuiltins;
    BitVector m_builtinFunctionsThatStayApart;
    BitVector m_bodiesOfLibraryModules;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
