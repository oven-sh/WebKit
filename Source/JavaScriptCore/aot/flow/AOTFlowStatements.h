/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTFlowIDs.h"
#include "AOTSchemaEnums.h"
#include <array>
#include <optional>
#include <span>
#include <wtf/HashTraits.h>
#include <wtf/OptionSet.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

enum class FlowSeedKind : uint8_t {
    Primitives,
    Integers,
    String,
    Symbol,
    Site,
    Builtin,
    ModuleBuiltin,
    Namespace,
    TypeAsRead,
    Unknown,
};

class FlowSeed {
public:
    static FlowSeed primitives(FlowLocalID node, uint16_t bits) { return { node, FlowSeedKind::Primitives, bits }; }
    static FlowSeed integers(FlowLocalID node, uint32_t rangeIndex) { return { node, FlowSeedKind::Integers, rangeIndex }; }
    static FlowSeed string(FlowLocalID node, StringID string) { return { node, FlowSeedKind::String, string.index() }; }
    static FlowSeed symbol(FlowLocalID node, PropertyKeyID key) { return { node, FlowSeedKind::Symbol, key.index() }; }
    static FlowSeed site(FlowLocalID node, uint32_t siteInSummary) { return { node, FlowSeedKind::Site, siteInSummary }; }
    static FlowSeed builtin(FlowLocalID node, BuiltinID builtin) { return { node, FlowSeedKind::Builtin, builtin.index() }; }
    static FlowSeed moduleBuiltin(FlowLocalID node, BuiltinID builtin, ModuleID module) { return { node, FlowSeedKind::ModuleBuiltin, builtin.index(), module.index() }; }
    static FlowSeed moduleNamespace(FlowLocalID node, ModuleID module) { return { node, FlowSeedKind::Namespace, module.index() }; }
    static FlowSeed typeAsRead(FlowLocalID node, TypeID type) { return { node, FlowSeedKind::TypeAsRead, type.index() }; }
    static FlowSeed unknown(FlowLocalID node) { return { node, FlowSeedKind::Unknown, 0 }; }

    FlowLocalID node() const { return m_node; }
    FlowSeedKind kind() const { return m_kind; }

    uint16_t primitives() const { return static_cast<uint16_t>(payload(FlowSeedKind::Primitives)); }
    uint32_t rangeIndex() const { return payload(FlowSeedKind::Integers); }
    StringID string() const { return StringID(payload(FlowSeedKind::String)); }
    PropertyKeyID symbol() const { return PropertyKeyID(payload(FlowSeedKind::Symbol)); }
    uint32_t site() const { return payload(FlowSeedKind::Site); }
    TypeID type() const { return TypeID(payload(FlowSeedKind::TypeAsRead)); }

    BuiltinID builtin() const
    {
        RELEASE_ASSERT(m_kind == FlowSeedKind::Builtin || m_kind == FlowSeedKind::ModuleBuiltin);
        return BuiltinID(m_first);
    }

    ModuleID module() const
    {
        RELEASE_ASSERT(m_kind == FlowSeedKind::ModuleBuiltin || m_kind == FlowSeedKind::Namespace);
        return ModuleID(m_kind == FlowSeedKind::Namespace ? m_first : m_second);
    }

    FlowSeed withNode(FlowLocalID node) const { return { node, m_kind, m_first, m_second }; }

    friend bool operator==(const FlowSeed&, const FlowSeed&) = default;

private:
    FlowSeed(FlowLocalID node, FlowSeedKind kind, uint32_t first, uint32_t second = 0)
        : m_node(node)
        , m_kind(kind)
        , m_first(first)
        , m_second(second)
    {
    }

    uint32_t payload(FlowSeedKind kind) const
    {
        RELEASE_ASSERT(m_kind == kind);
        return m_first;
    }

    FlowLocalID m_node;
    FlowSeedKind m_kind;
    uint32_t m_first;
    uint32_t m_second;
};

static_assert(sizeof(FlowSeed) == 16);

struct FlowLocalList {
    uint32_t first { 0 };
    uint32_t count { 0 };
};

enum class FlowFilterKind : uint8_t { Inside, Outside };

struct FlowFilter {
    // The solver makes an edge of such a filter, and runs every other one as a statement.
    bool isEdge() const { return kind == FlowFilterKind::Inside && !field; }

    FlowLocalID from;
    FlowLocalID to;
    TypeID type;
    FlowFilterKind kind { FlowFilterKind::Inside };
    // With a name, an object passes by what a read of that name from it may give, and everything else passes. The range of the summary stands in for the type.
    PropertyKeyID field;
    std::optional<uint32_t> range;
};

struct FlowConditionalSeed {
    FlowLocalID from;
    TypeID type;
    FlowFilterKind kind { FlowFilterKind::Inside };
    ValueID origin;
    FlowSeed seed;
};

// What is computed from the operands of `origin`: its result, or one of the operands as it is behind the branch on that result.
enum class FlowNaturalKind : uint8_t { Result, OperandOnTakenEdge, OperandOnNotTakenEdge };

struct FlowNaturalOperation {
    ValueID origin;
    FlowLocalID result;
    FlowLocalList operands;
    FlowNaturalKind kind { FlowNaturalKind::Result };
    uint8_t narrowedOperand { 0 };
};

struct FlowAllocation {
    ValueID origin;
    uint32_t site { 0 };
    FlowLocalID prototype;
    bool isInCycle { true };
};

enum class FlowAccessKind : uint8_t {
    Load,
    Store,
    Define,
    DefineGetter,
    DefineSetter,
    Delete,
    Test,
};

constexpr bool isWrite(FlowAccessKind kind)
{
    switch (kind) {
    case FlowAccessKind::Store:
    case FlowAccessKind::Define:
    case FlowAccessKind::DefineGetter:
    case FlowAccessKind::DefineSetter:
    case FlowAccessKind::Delete:
        return true;
    case FlowAccessKind::Load:
    case FlowAccessKind::Test:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

enum class FlowReadUse : uint8_t { Value, CalleeOnly };

enum class FlowAccessFlag : uint16_t {
    StartsAtPrototype = 1 << 0,
    IsFreeName = 1 << 1,
    IsForTypeOf = 1 << 2,
    IsSloppy = 1 << 3,
    IsInCycle = 1 << 4,
    IsOwnOnly = 1 << 5,
    NamesReceivers = 1 << 6,
    StoresUnobservedValue = 1 << 7,
    // The key is a length that the receiver had, or was found below one.
    IsWithinLength = 1 << 8,
};

struct FlowLocationAccess {
    FlowAccessKind kind { FlowAccessKind::Load };
    FlowReadUse use { FlowReadUse::Value };
    bool isInCycle { true };
    ValueID origin;
    FlowLocalID location;
    FlowLocalID value;
};

struct FlowFieldAccess {
    FlowAccessKind kind { FlowAccessKind::Load };
    FlowReadUse use { FlowReadUse::Value };
    bool isInCycle { true };
    ValueID origin;
    FlowLocalID receiver;
    LayoutID layout;
    uint32_t field { 0 };
    FlowLocalID value;
    FlowLocalID thrown;
};

struct FlowPropertyAccess {
    FlowAccessKind kind { FlowAccessKind::Load };
    FlowReadUse use { FlowReadUse::Value };
    OptionSet<FlowAccessFlag> flags;
    ValueID origin;
    FlowLocalID receiver;
    PropertyKeyID key;
    FlowLocalID value;
    FlowLocalID thisValue;
    FlowLocalID thrown;
};

struct FlowElementAccess {
    FlowAccessKind kind { FlowAccessKind::Load };
    FlowReadUse use { FlowReadUse::Value };
    OptionSet<FlowAccessFlag> flags;
    ValueID origin;
    FlowLocalID receiver;
    FlowLocalID key;
    FlowLocalID value;
    FlowLocalID thisValue;
    FlowLocalID thrown;
};

struct FlowSlotAccess {
    FlowAccessKind kind { FlowAccessKind::Load };
    ValueID origin;
    FlowLocalID receiver;
    std::optional<ContentsSlot> contentsSlot;
    BuiltinSlotID slot;
    std::optional<uint32_t> internalFieldIndex;
    std::optional<uint32_t> position;
    FlowLocalID value;
    bool mayLeaveHole { false };
};

struct FlowPrototypeAccess {
    FlowAccessKind kind { FlowAccessKind::Load };
    ValueID origin;
    FlowLocalID object;
    FlowLocalID value;
};

struct FlowPropertyCopy {
    ValueID origin;
    FlowLocalID source;
    FlowLocalID target;
    FlowLocalList excludedKeys;
    FlowLocalID thrown;
};

enum class FlowCallMode : uint8_t { Call, Construct, SuperConstruct };

enum class FlowCallFlag : uint8_t {
    IsInCycle = 1 << 0,
    IsTailCall = 1 << 1,
    IsAnnotatedPure = 1 << 2,
    // A store of the function names what the call gave as its receiver.
    ResultGetsProperties = 1 << 3,
    // The function returns what the call gave.
    ResultIsHandedUp = 1 << 4,
};

struct FlowCall {
    FlowCallMode mode { FlowCallMode::Call };
    OptionSet<FlowCallFlag> flags;
    ValueID origin;
    FlowLocalID callee;
    FlowLocalID thisValue;
    FlowLocalID newTarget;
    FlowLocalList arguments;
    FlowLocalID argumentList;
    FlowLocalID result;
    FlowLocalID thrown;
    LayoutID derivedLayout;
    ExpectedResultID expectedResult;
    // Of a call of a member. The callees are what a lookup under the key finds on each receiver, and the callee operand, which holds all of them together, is not read.
    PropertyKeyID calleeKey;
};

struct FlowDirectCall {
    OptionSet<FlowCallFlag> flags;
    ValueID origin;
    FunctionID function;
    FlowLocalList arguments;
    FlowLocalList results;
    FlowLocalID thrown;
};

struct FlowClassDefinition {
    ValueID origin;
    LayoutID layout;
    FlowLocalID constructor;
    FlowLocalID heritage;
    FlowLocalID prototypeParent;
    bool isInCycle { true };
};

struct FlowMethodDefinition {
    ValueID origin;
    LayoutID layout;
    ClassSide side { ClassSide::Prototype };
    MethodKind methodKind { MethodKind::Method };
    uint32_t index { 0 };
    FlowLocalID classObject;
    FlowLocalID function;
    PropertyKeyID key;
    FlowLocalID computedKey;
};

struct FlowAwait {
    ValueID origin;
    FlowLocalID value;
    FlowLocalID result;
    FlowLocalID thrown;
};

enum class FlowIterationStep : uint8_t { Open, Resume, Close };

struct FlowIteration {
    FlowIterationStep step { FlowIterationStep::Open };
    IteratorKind iteratorKind { IteratorKind::Sync };
    ResumeMode resumeMode { ResumeMode::Next };
    ValueID origin;
    FlowLocalID iterable;
    FlowLocalID state;
    FlowLocalID sent;
    FlowLocalID value;
    FlowLocalID thrown;
    std::optional<uint32_t> stepIndex;
};

struct FlowOperandUse {
    ValueID origin;
    uint32_t operandIndex { 0 };
    FlowLocalID operand;
    FlowLocalID retainer;
    FlowLocalID result;
    FlowLocalID thrown;
    bool isTerminatorOperand { false };
};

enum class FlowModuleOperationKind : uint8_t {
    Namespace,
    Import,
    Require,
    ImportDynamic,
    RequireDynamic,
    RequireBuiltinModule,
    ResolveDynamic,
    Use,
};

struct FlowModuleOperation {
    FlowModuleOperationKind kind { FlowModuleOperationKind::Namespace };
    ValueID origin;
    ModuleID module;
    RuntimeModuleID runtimeModule;
    FlowLocalID specifier;
    FlowLocalID result;
    FlowLocalID thrown;
};

struct FlowUnseenCodeEvent {
    ValueID origin;
    UnseenCodeCause cause { UnseenCodeCause::Unknown };
};

// An instance first runs its statements in this order. The prototype of a literal is stored before any access walks from the literal and seeds the default prototype.
#define FOR_EACH_FLOW_STATEMENT_KIND(v) \
    v(Filter, FlowFilter, filters, 1) \
    v(ConditionalSeed, FlowConditionalSeed, conditionalSeeds, 0) \
    v(NaturalOperation, FlowNaturalOperation, naturalOperations, 0) \
    v(Allocation, FlowAllocation, allocations, 0) \
    v(PrototypeAccess, FlowPrototypeAccess, prototypeAccesses, 2) \
    v(LocationAccess, FlowLocationAccess, locationAccesses, 0) \
    v(FieldAccess, FlowFieldAccess, fieldAccesses, 1) \
    v(PropertyAccess, FlowPropertyAccess, propertyAccesses, 1) \
    v(ElementAccess, FlowElementAccess, elementAccesses, 1) \
    v(SlotAccess, FlowSlotAccess, slotAccesses, 1) \
    v(PropertyCopy, FlowPropertyCopy, propertyCopies, 2) \
    v(Call, FlowCall, calls, 2) \
    v(DirectCall, FlowDirectCall, directCalls, 0) \
    v(ClassDefinition, FlowClassDefinition, classDefinitions, 2) \
    v(MethodDefinition, FlowMethodDefinition, methodDefinitions, 1) \
    v(Await, FlowAwait, awaits, 1) \
    v(Iteration, FlowIteration, iterations, 1) \
    v(OperandUse, FlowOperandUse, operandUses, 0) \
    v(ModuleOperation, FlowModuleOperation, moduleOperations, 0) \
    v(UnseenCodeEvent, FlowUnseenCodeEvent, unseenCodeEvents, 0)

enum class FlowStatementKind : uint8_t {
#define FLOW_STATEMENT_ENUMERATOR(kind, type, member, cursors) kind,
    FOR_EACH_FLOW_STATEMENT_KIND(FLOW_STATEMENT_ENUMERATOR)
#undef FLOW_STATEMENT_ENUMERATOR
    // No summary has one. The solver makes it when a statement calls a built-in or a bound function, and its index is program-wide.
    BuiltinCall,
    // No summary has one, and it neither runs nor is woken. The walk of a kept lookup waits in its name, which only lists the waits.
    Lookup,
};

inline constexpr unsigned numberOfSummaryStatementKinds = 0
#define FLOW_STATEMENT_COUNT(kind, type, member, cursors) + 1
    FOR_EACH_FLOW_STATEMENT_KIND(FLOW_STATEMENT_COUNT)
#undef FLOW_STATEMENT_COUNT
    ;

constexpr uint32_t numberOfCursorsOf(FlowStatementKind statementKind)
{
    switch (statementKind) {
#define FLOW_STATEMENT_CURSORS(kind, type, member, cursors) \
    case FlowStatementKind::kind: \
        return cursors;
        FOR_EACH_FLOW_STATEMENT_KIND(FLOW_STATEMENT_CURSORS)
#undef FLOW_STATEMENT_CURSORS
    case FlowStatementKind::BuiltinCall:
    case FlowStatementKind::Lookup:
        return 0;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

struct FlowStatementReference {
    FlowStatementKind kind { FlowStatementKind::Filter };
    uint32_t index { 0 };

    friend bool operator==(const FlowStatementReference&, const FlowStatementReference&) = default;
};

struct FlowStatementSite {
    FlowInstanceID instance;
    FlowStatementReference statement;

    friend bool operator==(const FlowStatementSite&, const FlowStatementSite&) = default;
};

inline void add(Hasher& hasher, const FlowStatementSite& site)
{
    add(hasher, site.instance, static_cast<uint8_t>(site.statement.kind), site.statement.index);
}

} } } // namespace JSC::AOT::Typed

namespace WTF {

template<> struct HashTraits<JSC::AOT::Typed::FlowStatementSite> : GenericHashTraits<JSC::AOT::Typed::FlowStatementSite> {
    using FlowStatementSite = JSC::AOT::Typed::FlowStatementSite;
    using FlowInstanceID = JSC::AOT::Typed::FlowInstanceID;

    static void constructDeletedValue(FlowStatementSite& slot)
    {
        slot = FlowStatementSite { };
        HashTraits<FlowInstanceID>::constructDeletedValue(slot.instance);
    }

    static bool isDeletedValue(const FlowStatementSite& site) { return HashTraits<FlowInstanceID>::isDeletedValue(site.instance); }
};

} // namespace WTF

namespace JSC { namespace AOT { namespace Typed {

struct FlowLocalLists {
    Vector<FlowLocalID> localLists;

    std::span<const FlowLocalID> list(FlowLocalList localList) const LIFETIME_BOUND
    {
        RELEASE_ASSERT(static_cast<uint64_t>(localList.first) + localList.count <= localLists.size());
        return localLists.subspan(localList.first, localList.count);
    }

    FlowLocalList addList(std::span<const FlowLocalID> locals)
    {
        FlowLocalList localList { static_cast<uint32_t>(localLists.size()), static_cast<uint32_t>(locals.size()) };
        localLists.append(locals);
        return localList;
    }
};

struct FlowStatements : FlowLocalLists {
#define FLOW_STATEMENT_VECTOR(kind, type, member, cursors) Vector<type> member;
    FOR_EACH_FLOW_STATEMENT_KIND(FLOW_STATEMENT_VECTOR)
#undef FLOW_STATEMENT_VECTOR

    void forEach(NOESCAPE const Invocable<void(FlowStatementReference)> auto& functor) const
    {
#define VISIT_FLOW_STATEMENT_KIND(kind, type, member, cursors) \
        for (uint32_t index = 0; index < member.size(); ++index) \
            functor(FlowStatementReference { FlowStatementKind::kind, index });
        FOR_EACH_FLOW_STATEMENT_KIND(VISIT_FLOW_STATEMENT_KIND)
#undef VISIT_FLOW_STATEMENT_KIND
    }
};

struct FlowStatementNumbering {
    std::array<uint32_t, numberOfSummaryStatementKinds> firstStatementOfKind { };
    std::array<uint32_t, numberOfSummaryStatementKinds> firstCursorOfKind { };
    uint32_t numberOfStatements { 0 };
    uint32_t numberOfCursors { 0 };

    static FlowStatementNumbering of(const FlowStatements& statements)
    {
        FlowStatementNumbering numbering;
#define NUMBER_FLOW_STATEMENTS(kind, type, member, cursors) \
        numbering.firstStatementOfKind[static_cast<unsigned>(FlowStatementKind::kind)] = numbering.numberOfStatements; \
        numbering.numberOfStatements += static_cast<uint32_t>(statements.member.size()); \
        numbering.firstCursorOfKind[static_cast<unsigned>(FlowStatementKind::kind)] = numbering.numberOfCursors; \
        numbering.numberOfCursors += static_cast<uint32_t>(statements.member.size()) * cursors;
        FOR_EACH_FLOW_STATEMENT_KIND(NUMBER_FLOW_STATEMENTS)
#undef NUMBER_FLOW_STATEMENTS
        return numbering;
    }

    uint32_t numberOf(FlowStatementReference statement) const
    {
        return firstStatementOfKind[static_cast<unsigned>(statement.kind)] + statement.index;
    }

    uint32_t cursorOf(FlowStatementReference statement, uint32_t cursorInStatement) const
    {
        uint32_t cursorsPerStatement = numberOfCursorsOf(statement.kind);
        RELEASE_ASSERT(cursorInStatement < cursorsPerStatement);
        return firstCursorOfKind[static_cast<unsigned>(statement.kind)] + statement.index * cursorsPerStatement + cursorInStatement;
    }
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
