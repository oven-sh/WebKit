/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTFlowObjectSet.h"
#include "AOTIRProgram.h"
#include <memory>
#include <optional>
#include <span>
#include <wtf/Forward.h>
#include <wtf/Noncopyable.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

enum class FlowObjectKind : uint8_t {
    Unknown,
    Allocation,
    Closure,
    Prototype,
    Instance,
    Opaque,
    ModuleNamespace,
    Builtin,
    ModuleBuiltin,
};

// Whether a receiver escapes when a lookup on it may find an accessor or a method that the build does not see.
constexpr bool canEscapeAsReceiver(FlowObjectKind kind)
{
    switch (kind) {
    case FlowObjectKind::Allocation:
    case FlowObjectKind::Closure:
    case FlowObjectKind::Prototype:
    case FlowObjectKind::Instance:
    case FlowObjectKind::Opaque:
        return true;
    case FlowObjectKind::Unknown:
    case FlowObjectKind::ModuleNamespace:
    case FlowObjectKind::Builtin:
    case FlowObjectKind::ModuleBuiltin:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

enum class FlowSiteKind : uint8_t {
    Layout,
    Closure,
    Instance,
    Opaque,
    PlainObject,
    PlainArray,
};

enum class FlowSiteOrigin : uint8_t {
    Operation,
    DataNode,
    CoroutineResult,
    RestParameter,
    BuiltinResult,
    BuiltinElement,
    BuiltinConstructor,
    TypedResult,
    RuntimeError,
};

// What defines a property of an object literal behind its allocation. Each takes the allocation as this operand.
static constexpr OpcodeSet literalPieceOperations { Opcode::PropDefine, Opcode::ElemDefine, Opcode::DefineGetter, Opcode::DefineSetter, Opcode::CopyDataProperties };
static constexpr uint32_t literalOperandOfPiece = OperandIndex::FinishObject::object;

static_assert(OperandIndex::PropDefine::object == literalOperandOfPiece && OperandIndex::ElemDefine::object == literalOperandOfPiece);
static_assert(OperandIndex::DefineGetter::object == literalOperandOfPiece && OperandIndex::DefineSetter::object == literalOperandOfPiece);
static_assert(OperandIndex::CopyDataProperties::target == literalOperandOfPiece);

struct FlowSiteDescription {
    FlowSiteKind kind { FlowSiteKind::Opaque };
    FlowSiteOrigin origin { FlowSiteOrigin::Operation };
    uint16_t numberOfPositions { 0 };
    // Of an array: every index below the length it is created with holds a value.
    bool isCreatedDense { false };
    // Of an object literal: operations between the allocation and FinishObject define more of its properties.
    bool isCompletedByPieces { false };
    ValueID value;
    LayoutID layout;
    BuiltinClassID builtinClass;
    FunctionID closureFunction;
    uint32_t ordinal { 0 };

    friend bool operator==(const FlowSiteDescription&, const FlowSiteDescription&) = default;
};

// The result of a built-in has a layout of the table, and the instance of a class that extends a built-in class has the layout of the class, so that the solver follows its fields.
// The built-in's own code makes the object: an ordinary object, or a cell of the built-in class, whose properties are found by name.
constexpr bool allocatesOrdinaryObject(const FlowSiteDescription& description)
{
    if (description.kind != FlowSiteKind::Layout)
        return false;
    switch (description.origin) {
    case FlowSiteOrigin::BuiltinResult:
    case FlowSiteOrigin::BuiltinElement:
    case FlowSiteOrigin::BuiltinConstructor:
        return true;
    case FlowSiteOrigin::Operation:
    case FlowSiteOrigin::DataNode:
    case FlowSiteOrigin::CoroutineResult:
    case FlowSiteOrigin::RestParameter:
    case FlowSiteOrigin::TypedResult:
    case FlowSiteOrigin::RuntimeError:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

struct FlowSite {
    FunctionID function;
    FlowSiteDescription description;
    FlowObjectID sharedObject;
};

struct FlowObject {
    FlowObjectKind kind { FlowObjectKind::Unknown };
    bool standsForLayout { false };
    bool hasUndeclaredPart { false };
    uint16_t numberOfPositions { 0 };
    uint32_t numberOfDeclaredParts { 0 };
    AllocationSiteID site;
    AllocationClassID allocationClass;
    LayoutID layout;
    BuiltinClassID builtinClass;
    BuiltinID builtin;
    FunctionID function;
    ModuleID module;
    FlowObjectID owner;
    FlowInstanceID instance;

    uint32_t numberOfParts() const { return numberOfDeclaredParts + numberOfPositions + (hasUndeclaredPart ? 1 : 0); }
    bool isCopy() const { return !!instance; }
};

struct FlowAllocationClassMembers {
    LayoutID layout;
    BuiltinClassID builtinClass;
    FunctionID closureFunction;
    FlowObjectID sharedObject;
    Vector<AllocationSiteID, 1> sites;
    Vector<FlowObjectID> copies;
};

struct FlowObjectIndexes;

struct FlowObjectIndexesDeleter {
    void operator()(FlowObjectIndexes*) const;
};

class FlowObjectTable {
    WTF_MAKE_TZONE_ALLOCATED(FlowObjectTable);
    WTF_MAKE_NONCOPYABLE(FlowObjectTable);
public:
    explicit FlowObjectTable(const Program&);
    ~FlowObjectTable();

    const Program& program() const { return m_program; }

    unsigned size() const { return m_objects.size(); }
    const FlowObject& at(FlowObjectID object) const LIFETIME_BOUND { return m_objects[object.index()]; }
    FlowObjectKind kindOf(FlowObjectID object) const { return m_objects[object.index()].kind; }

    unsigned numberOfSites() const { return m_sites.size(); }
    const FlowSite& at(AllocationSiteID site) const LIFETIME_BOUND { return m_sites[site.index()]; }
    AllocationSiteID siteOf(FunctionID, uint32_t siteInSummary) const;
    uint32_t numberOfSitesOf(FunctionID) const;

    unsigned numberOfAllocationClasses() const { return m_classes.size(); }
    const FlowAllocationClassMembers& at(AllocationClassID allocationClass) const LIFETIME_BOUND { return m_classes[allocationClass.index()]; }
    std::span<const AllocationClassID> allocationClassesOf(LayoutID) const LIFETIME_BOUND;
    // The layout that the allocation of the object names. The record of the object keeps the declared layout, which numbers its declared parts.
    LayoutID allocatedLayoutOf(FlowObjectID) const;
    std::span<const LayoutID> derivedLayoutsOf(LayoutID declared) const LIFETIME_BOUND;
    bool isMadeByBuiltinConstructor(FlowObjectID) const;
    FlowObjectID layoutObject(LayoutID) const;

    FlowObjectID findBuiltin(BuiltinID) const;
    FlowObjectID findModuleBuiltin(BuiltinID, ModuleID) const;
    FlowObjectID findNamespace(ModuleID) const;
    FlowObjectID findPrototype(FlowObjectID closure) const;
    std::span<const FlowObjectID> closuresOf(FunctionID) const LIFETIME_BOUND;

    bool hasLayout(FlowObjectID object) const { return !!at(object).layout && kindOf(object) == FlowObjectKind::Allocation; }
    bool isInstanceOf(FlowObjectID, BuiltinClassID) const;
    bool isCallable(FlowObjectID) const;
    // Only a call of the object reads its declared parts, and nothing writes them.
    bool hasCaptures(FlowObjectID) const;
    FunctionID allocatingFunctionOf(FlowObjectID) const;
    Wire::SourceLocation locationOf(FlowObjectID) const;

    FlowObjectID append(const FlowObject&);
    AllocationSiteID appendSite(const FlowSite&);
    AllocationClassID appendAllocationClass(FlowAllocationClassMembers&&);
    void moveToDerivedLayout(AllocationClassID, LayoutID derived);
    // The objects come in runs of equal first members. The first run stays in the class, and each other run gets a class of its own with the same sites.
    void splitAllocationClass(AllocationClassID, std::span<const std::pair<uint32_t, FlowObjectID>> objectsInRuns);
    FlowObject& mutableAt(FlowObjectID object) LIFETIME_BOUND { return m_objects[object.index()]; }
    FlowSite& mutableAt(AllocationSiteID site) LIFETIME_BOUND { return m_sites[site.index()]; }
    FlowAllocationClassMembers& mutableAt(AllocationClassID allocationClass) LIFETIME_BOUND { return m_classes[allocationClass.index()]; }
    void setFirstSiteOfFunctions(Vector<uint32_t>&& firstSiteByFunction) { m_firstSiteOfFunction = WTF::move(firstSiteByFunction); }

    FlowObjectIndexes& indexes() { return *m_indexes; }
    const FlowObjectIndexes& indexes() const { return *m_indexes; }

    void dump(PrintStream&, FlowObjectID) const;

private:
    const Program& m_program;
    Vector<FlowObject> m_objects;
    Vector<FlowSite> m_sites;
    Vector<uint32_t> m_firstSiteOfFunction;
    Vector<FlowAllocationClassMembers> m_classes;
    std::unique_ptr<FlowObjectIndexes, FlowObjectIndexesDeleter> m_indexes;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
