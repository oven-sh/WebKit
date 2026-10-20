/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFunctionTables.h"
#include "AOTLayoutData.h"
#include "CollectionScope.h"
#include "JSCJSValue.h"
#include "PropertyOffset.h"
#include "RememberedSlot.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <optional>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>
#include <wtf/text/UniquedStringImpl.h>

namespace JSC {

class GetterSetter;
class JSFinalObject;
class JSGlobalObject;
class JSObject;
class Structure;
class VM;

namespace AOT {

class Closure;
class Object;
struct State;

class ClassTable {
    WTF_MAKE_NONCOPYABLE(ClassTable);
public:
    explicit ClassTable(State&);
    ~ClassTable();

    const LayoutData* classWithIndex(uint32_t) const;
    const LayoutData* classConstructedBy(FunctionID) const;
    const LayoutData* baseOf(const LayoutData& layout) const { return layout.base() == LayoutData::noBase ? nullptr : classWithIndex(layout.base()); }
    const LayoutData& classOfInstances(const LayoutData& instanceLayout) const
    {
        const LayoutData* current = &instanceLayout;
        while (current->isDerived && current->isClass())
            current = baseOf(*current);
        return *current;
    }
    // The proven base, else the predicted one, which holds only while the class's link guards are set.
    const LayoutData* assumedBaseOf(const LayoutData& layout) const { return layout.assumedBase() == LayoutData::noBase ? nullptr : classWithIndex(layout.assumedBase()); }
    static bool hasOneClassObject(const LayoutData& classLayout) { return classLayout.evaluation() == EvaluationState::ProvenOnce && !captureCountsOf(FunctionID(classLayout.classData().classConstructor)).captures; }
    static bool hasComputedMemberNames(const LayoutData&);
    static bool isDefinedByClass(const LayoutData&);
    bool hasStaticMethodNamed(const LayoutData&, UniquedStringImpl*) const;
    const FieldData* staticField(const LayoutData&, UniquedStringImpl*) const;
    JSValue readStaticField(const FieldData&) const;
    bool tryStoreStaticField(VM&, const FieldData&, JSValue);

    JSObject* classPrototype(VM&, const LayoutData&);
    JSObject* classPrototypeIfExists(const LayoutData&) const;
    JSObject* createClassPrototype(VM&, const LayoutData&, JSObject* parent);
    void registerClassPrototype(const LayoutData&, JSObject*);
    Structure* lazyClassObjectStructure(VM&, const LayoutData&);
    Structure* lazyClassObjectStructureInheritingFrom(VM&, JSObject* parent);
    size_t removeDeadEntries(VM&, CollectionScope);

    enum class ClassPart : uint8_t { Everything, ClassObject, PrototypeMembers };
    JSValue resolveHeritage(JSGlobalObject*, JSValue heritage);
    void defineClass(JSGlobalObject*, Closure* classObject, JSValue parent, JSValue prototypeParent);
    void createClassPart(JSGlobalObject*, Closure* classObject, ClassPart);
    JSObject* createMethodClosure(VM&, Closure* classObject, const LayoutData&, const MethodData&);
    // For a class proven to be evaluated once, before the run that evaluates it starts again. After the first call, what the earlier run made asks this table for nothing.
    void finishLazyParts(VM&, const LayoutData&);
    void forgetDefinition(VM&, const LayoutData&);

    uint64_t* guardWords(const LayoutData&, ClassSide) const;
    void armMemberGuards(VM&, const LayoutData&, ClassSide, JSObject* holder);
    void disarmMemberGuards(const LayoutData&);
    void didChangeMember(const LayoutData&, ClassSide, UniquedStringImpl*);
    void didDefineMember(VM&, const LayoutData&, ClassSide, JSObject* holder, UniquedStringImpl*);
    void didChangeEveryMember(const LayoutData&, ClassSide);
    void didChangePrototypeLink(const LayoutData&, ClassSide);
    const LayoutData* classHeldBy(JSObject* holder, ClassSide&) const;

    bool declaresOwnMember(const LayoutData&, UniquedStringImpl*) const;
    std::optional<JSObject*> findDeclaredMethod(VM&, const LayoutData&, UniquedStringImpl*);
    GetterSetter* createDeclaredAccessor(VM&, const LayoutData&, UniquedStringImpl*);
    struct DeclaredMember {
        void define(const MethodData&);
        bool isAccessor() const { return getter || setter; }

        const MethodData* method { nullptr };
        const MethodData* getter { nullptr };
        const MethodData* setter { nullptr };
    };
    struct DeclaredMemberLocation {
        JSObject* holder;
        PropertyOffset offset;
        bool isAccessor;
    };
    struct LazyPrototypeLookupResult {
        std::optional<DeclaredMemberLocation> member;
        JSObject* firstObjectAbove;
    };
    enum class MemberCreation : uint8_t { Allowed, Forbidden };
    std::optional<LazyPrototypeLookupResult> findThroughLazyPrototypes(VM&, JSObject* base, UniquedStringImpl*, bool isAbsentFromBase, MemberCreation = MemberCreation::Allowed);
    bool hasPrototypeMembers(const LayoutData&) const;

    JSObject* createThis(JSGlobalObject*, JSObject* constructor, JSObject* newTarget);
    JSObject* prototypeForInstancesOf(VM&, JSObject* compiledCallee);
    JSObject* instanceWithPrototype(VM&, Object* allocated, JSObject* prototype);

private:
    using ByIndex = UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;
    static constexpr unsigned declaredMembersPerHolder = 62;

    DeclaredMemberLocation declaredMemberLocationAt(uint32_t location, bool isAccessor) const;
    std::optional<DeclaredMemberLocation> findDeclaredMemberLocation(FunctionID key, bool isAccessor) const;
    DeclaredMemberLocation ensureDeclaredMemberLocation(VM&, FunctionID);
    DeclaredMemberLocation ensureDeclaredMemberLocation(VM&, const DeclaredMember& accessor);
    template<typename CreateMember> DeclaredMemberLocation ensureDeclaredMemberLocation(VM&, FunctionID key, bool isAccessor, const CreateMember&);
    Object* createClassInstance(VM&, const LayoutData&, JSObject* prototype);
    static JSFinalObject* createOrdinaryInstance(JSGlobalObject*, unsigned numberOfProperties, JSObject* prototype);
    const LayoutData* classWithPrototype(JSObject* prototype) const;
    JSObject* defineClassOfConstructorFunction(VM&, const LayoutData&);

    State& m_state;
    const ProgramTables& m_tables;
    mutable std::array<std::atomic<uint64_t>, 64> m_recentClassLookups { };
    UncheckedKeyHashMap<JSObject*, Structure*> m_classPrototypeStructures;
    UncheckedKeyHashMap<JSObject*, Structure*> m_lazyClassObjectStructures;
    UncheckedKeyHashMap<JSObject*, uint32_t> m_classSidesByHolder;
    RememberedSlotSet m_writes;
    Vector<JSObject*> m_declaredMemberHolders;
    ByIndex m_declaredMemberLocations;
    uint32_t m_numberOfDeclaredMemberLocations { 0 };
    UncheckedKeyHashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_classesWithForgottenDefinition;
};

// The other classes are those of constructor functions.
inline bool ClassTable::isDefinedByClass(const LayoutData& layout)
{
    if (!layout.isClass())
        return false;
    FunctionID constructor(layout.classData().classConstructor);
    return constructor && functionKindData(constructor).isConstructor;
}

inline bool ClassTable::hasComputedMemberNames(const LayoutData& classLayout)
{
    auto hasComputedName = [](const MethodData& method) {
        return !!(method.flags & methodHasComputedName);
    };
    return std::ranges::any_of(classLayout.methods(), hasComputedName) || std::ranges::any_of(classLayout.staticMethods(), hasComputedName);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
