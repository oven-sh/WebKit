/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTBuiltinTableEnums.h"
#include "AOTSchemaEnums.h"
#include "AOTVector.h"
#include "ArrayBufferSharingMode.h"
#include "IterationKind.h"
#include "JSExportMacros.h"
#include "JSType.h"
#include "TypedArrayType.h"
#include <array>
#include <optional>
#include <span>
#include <wtf/EnumSet.h>
#include <wtf/Noncopyable.h>
#include <wtf/OptionSet.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/StringView.h>
#include <wtf/text/WTFString.h>

namespace JSC {

class Identifier;
class JSGlobalObject;
class JSValue;
class Structure;
class VM;
enum class LinkTimeConstant : int32_t;

namespace AOT {

using OperandUses = OptionSet<OperandUse>;
using Protocols = OptionSet<Protocol>;
using AbstractHeaps = EnumSet<AbstractHeap>;
using PristineAspectSet = OptionSet<PristineAspects>;

constexpr bool isObjectKind(BuiltinKind kind)
{
    switch (kind) {
    case BuiltinKind::Namespace:
    case BuiltinKind::Constructor:
    case BuiltinKind::Function:
    case BuiltinKind::Prototype:
    case BuiltinKind::Method:
        return true;
    case BuiltinKind::Accessor:
    case BuiltinKind::DataProperty:
    case BuiltinKind::Constant:
    case BuiltinKind::NativeBinding:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

inline constexpr OptionSet<BuiltinPlatform> everyBuiltinPlatform { BuiltinPlatform::Darwin, BuiltinPlatform::Linux, BuiltinPlatform::Windows, BuiltinPlatform::FreeBSD, BuiltinPlatform::Android };

inline constexpr OptionSet<BuiltinArchitecture> everyBuiltinArchitecture { BuiltinArchitecture::ARM64, BuiltinArchitecture::X86_64 };

struct BuiltinTarget {
    BuiltinPlatform platform;
    BuiltinArchitecture architecture;

    friend constexpr bool operator==(BuiltinTarget, BuiltinTarget) = default;
};

constexpr BuiltinTarget targetOfThisBuild()
{
#if OS(ANDROID)
    constexpr BuiltinPlatform platform = BuiltinPlatform::Android;
#elif OS(DARWIN)
    constexpr BuiltinPlatform platform = BuiltinPlatform::Darwin;
#elif OS(LINUX)
    constexpr BuiltinPlatform platform = BuiltinPlatform::Linux;
#elif OS(WINDOWS)
    constexpr BuiltinPlatform platform = BuiltinPlatform::Windows;
#elif OS(FREEBSD)
    constexpr BuiltinPlatform platform = BuiltinPlatform::FreeBSD;
#else
#error "The table of built-ins has no platform for this operating system."
#endif
#if CPU(ARM64)
    constexpr BuiltinArchitecture architecture = BuiltinArchitecture::ARM64;
#elif CPU(X86_64)
    constexpr BuiltinArchitecture architecture = BuiltinArchitecture::X86_64;
#else
#error "The table of built-ins has no architecture for this processor."
#endif
    return { platform, architecture };
}

inline bool readsCharactersOfString(BuiltinConversion conversion)
{
    switch (conversion) {
    case BuiltinConversion::ToNumber:
    case BuiltinConversion::ToNumeric:
    case BuiltinConversion::ToPropertyKey:
    case BuiltinConversion::ToIntegerOrInfinity:
    case BuiltinConversion::ToLength:
    case BuiltinConversion::ToIndex:
    case BuiltinConversion::ToInt32:
    case BuiltinConversion::ToUint32:
    case BuiltinConversion::ToBigInt:
        return true;
    case BuiltinConversion::None:
    case BuiltinConversion::ToBoolean:
    case BuiltinConversion::ToString:
    case BuiltinConversion::ToObject:
    case BuiltinConversion::ToPrimitive:
    case BuiltinConversion::Dictionary:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

enum class BuiltinTypeFlag : uint8_t {
    IsAnything = 1 << 0,
    HasAnyObject = 1 << 1,
    HasAnyFunction = 1 << 2,
    HasPlainData = 1 << 3,
    HasOnlyIntegers = 1 << 4,
};

constexpr bool isCalledAfterReturn(BuiltinCallbackTimes times)
{
    return times == BuiltinCallbackTimes::Later || times == BuiltinCallbackTimes::PerEvent;
}

enum class RuntimeFunctionRole : uint8_t {
    EngineBuiltin,
    HostBuiltin,
    CompilerFunction,
    Model,
};

enum class RuntimeFunctionFlag : uint8_t {
    IsHiddenInStackTraces = 1 << 0,
    IsNeverInlined = 1 << 1,
    UsesStubConvention = 1 << 2,
};

enum class RuntimeModuleFlag : uint8_t {
    HasSource = 1 << 0,
    HasOpenNamespace = 1 << 1,
    ListsAllExports = 1 << 2,
    IsPublic = 1 << 3,
};

enum class BuiltinHostReadContext : uint8_t {
    EntryPoint,
    Worker,
    Preload,
    Plugin,
    TestFile,
};

struct BuiltinIntegerRange {
    int64_t minimum { 1 };
    int64_t maximum { 0 };

    constexpr bool isEmpty() const { return minimum > maximum; }
    friend constexpr bool operator==(BuiltinIntegerRange, BuiltinIntegerRange) = default;
};

struct BuiltinSlotContents {
    BuiltinSlotID slot { };
    BuiltinTypeID type { };
};

enum class BuiltinRecordPool : uint8_t { Names, Parameters, Transfers, Callbacks, HandlerCalls, PropertyActions, ResultProperties, SlotContents, TypeParts, TypeLiterals, DispatchBranches, MemberClasses, Places };

template<BuiltinRecordPool> struct BuiltinRecordsOfPool;

template<typename Record, BuiltinRecordPool pool>
class BuiltinRecordStretch {
public:
    using value_type = Record;

    constexpr BuiltinRecordStretch() = default;
    constexpr BuiltinRecordStretch(uint16_t first, uint16_t count)
        : m_first(first)
        , m_count(count)
    {
    }

    const Record* data() const { return BuiltinRecordsOfPool<pool>::records() + m_first; }
    constexpr size_t size() const { return m_count; }
    constexpr bool empty() const { return !m_count; }
    const Record* begin() const { return data(); }
    const Record* end() const { return data() + m_count; }
    const Record& operator[](size_t index) const { return span()[index]; }
    const Record& front() const { return span().front(); }
    const Record& back() const { return span().back(); }
    std::span<const Record> span() const { return { data(), m_count }; }
    std::span<const Record> first(size_t count) const { return span().first(count); }
    std::span<const Record> subspan(size_t offset, size_t count = std::dynamic_extent) const { return span().subspan(offset, count); }
    operator std::span<const Record>() const { return span(); }

private:
    uint16_t m_first { 0 };
    uint16_t m_count { 0 };
};

template<typename Record, BuiltinRecordPool pool>
class BuiltinRecordPointer {
public:
    constexpr BuiltinRecordPointer() = default;
    constexpr BuiltinRecordPointer(std::nullptr_t) { }
    explicit constexpr BuiltinRecordPointer(uint16_t index)
        : m_indexPlusOne(index + 1)
    {
    }

    const Record* get() const { return m_indexPlusOne ? BuiltinRecordsOfPool<pool>::records() + m_indexPlusOne - 1 : nullptr; }
    explicit constexpr operator bool() const { return !!m_indexPlusOne; }
    operator const Record*() const { return get(); }
    const Record* operator->() const { return get(); }
    const Record& operator*() const { return *get(); }

private:
    uint16_t m_indexPlusOne { 0 };
};

using BuiltinNameStretch = BuiltinRecordStretch<BuiltinName, BuiltinRecordPool::Names>;

struct BuiltinTypePart {
    BuiltinTypePartKind kind { BuiltinTypePartKind::Instance };
    PristineAspectSet pristine { };
    std::optional<BuiltinOperand> operand { };
    BuiltinClassID builtinClass { };
    BuiltinLayoutID layout { };
    BuiltinID builtin { };
    BuiltinSlotID slot { };
    BuiltinRecordStretch<BuiltinSlotContents, BuiltinRecordPool::SlotContents> contents { };
    bool isSettled { false };
    bool isOfClassItself { false };
};

struct BuiltinType {
    OptionSet<BuiltinTypeFlag> flags { BuiltinTypeFlag::IsAnything };
    OptionSet<BuiltinPrimitive> primitives { };
    BuiltinFreshness freshness { BuiltinFreshness::No };
    BuiltinIntegerRange integers { };
    BuiltinRecordStretch<BuiltinName, BuiltinRecordPool::TypeLiterals> stringLiterals { };
    BuiltinRecordStretch<BuiltinTypePart, BuiltinRecordPool::TypeParts> parts { };

    constexpr bool isAnything() const { return flags.contains(BuiltinTypeFlag::IsAnything); }
    constexpr bool isNever() const { return flags.isEmpty() && primitives.isEmpty() && stringLiterals.empty() && parts.empty(); }
    constexpr bool mayBeObject() const
    {
        return flags.containsAny({ BuiltinTypeFlag::IsAnything, BuiltinTypeFlag::HasAnyObject, BuiltinTypeFlag::HasAnyFunction, BuiltinTypeFlag::HasPlainData }) || !parts.empty();
    }
};

struct BuiltinPlace {
    BuiltinPlaceKind kind { BuiltinPlaceKind::None };
    std::optional<BuiltinOperand> operand { };
    BuiltinSlotID slot { };
    uint16_t fieldIndex { 0 };
    bool holdsOnlyWhatIsStored { false };
    BuiltinTypeID newValueType { };
    bool isSettled { false };
    BuiltinNameStretch memberPath { };
    uint16_t firstMemberStep { 0 };
    BuiltinName nativeCell { };
};
static_assert(sizeof(BuiltinPlace) == 22);

using BuiltinPlaceStretch = BuiltinRecordStretch<BuiltinPlace, BuiltinRecordPool::Places>;

struct BuiltinKey {
    BuiltinKeyKind kind { BuiltinKeyKind::None };
    std::optional<BuiltinOperand> operand { };
    BuiltinNameStretch names { };
};
static_assert(sizeof(BuiltinKey) == 8);

struct BuiltinTransfer {
    BuiltinPlace from { };
    BuiltinPlace to { };
};
static_assert(sizeof(BuiltinTransfer) == 44);

struct BuiltinCallback {
    BuiltinOperand callee;
    BuiltinCallbackTimes times { BuiltinCallbackTimes::Later };
    bool constructs { false };
    BuiltinPlace newTarget { };
    BuiltinPlace thisValue { };
    BuiltinPlaceStretch arguments { };
    BuiltinPlace spreadArguments { };
    BuiltinCallbackResultUse resultUse { BuiltinCallbackResultUse::Escapes };
    BuiltinPlace resultTo { };
    BuiltinCallbackAftermath afterwards { BuiltinCallbackAftermath::WritesIntoWhatItFinds };
};

struct BuiltinHandlerCall {
    BuiltinPlace callee { };
    BuiltinCallbackTimes times { BuiltinCallbackTimes::PerEvent };
    BuiltinPlace thisValue { };
    BuiltinPlaceStretch arguments { };
    BuiltinCallbackResultUse resultUse { BuiltinCallbackResultUse::Escapes };
};

struct BuiltinPropertyAction {
    BuiltinPropertyActionKind kind;
    BuiltinOperand object;
    BuiltinKey key { };
    BuiltinPlace value { };
    BuiltinIntegrityLevel integrityLevel { BuiltinIntegrityLevel::NonExtensible };
};

struct BuiltinParameter {
    BuiltinOperand operand;
    BuiltinParameterMode mode { BuiltinParameterMode::Required };
    BuiltinConversion conversion { BuiltinConversion::None };
    BuiltinTypeID accepted { };
    BuiltinTypeID converted { };
    BuiltinTypeID refused { };
    BuiltinTypeID admitted { };
    OperandUses uses { OperandUses::all() };
    OptionSet<BuiltinOperandRole> roles { };
    Protocols protocols { Protocols::all() };
    BuiltinNameStretch namedMembers { };
    BuiltinNameStretch calledMembers { };
    BuiltinNameStretch convertedMembers { };
};

struct BuiltinResultProperty {
    BuiltinName name;
    BuiltinTypeID type { };
};

struct BuiltinResult {
    BuiltinTypeID type { };
    BuiltinRangeRule rangeRule { BuiltinRangeRule::None };
    std::optional<BuiltinOperand> rangeOperand { };
    std::optional<BuiltinOperand> substringOf { };
    std::optional<BuiltinOperand> rangeOf { };
    TypedResultForm typedResultForm { TypedResultForm::None };
    std::optional<BuiltinOperand> typedResultSource { };
    std::optional<BuiltinOperand> iteratedOperand { };
    IterationKind iterationKind { IterationKind::Values };
    std::optional<BuiltinOperand> prototypeSource { };
    // It answers whether this operand was made as an instance of one of the classes of that type.
    std::optional<BuiltinOperand> testedOperand { };
    BuiltinTypeID testedType { };
    BuiltinRecordStretch<BuiltinResultProperty, BuiltinRecordPool::ResultProperties> properties { };
};

struct BuiltinEffects {
    BuiltinCapability capability { BuiltinCapability::JS };
    AbstractHeaps reads { };
    AbstractHeaps writes { };
};

struct BuiltinFlow {
    BuiltinRecordStretch<BuiltinTransfer, BuiltinRecordPool::Transfers> transfers { };
    BuiltinRecordStretch<BuiltinCallback, BuiltinRecordPool::Callbacks> callbacks { };
    BuiltinRecordStretch<BuiltinPropertyAction, BuiltinRecordPool::PropertyActions> propertyActions { };
    BuiltinRecordStretch<BuiltinHandlerCall, BuiltinRecordPool::HandlerCalls> handlerCalls { };
};

using RealmStructureID = BuiltinTableID<struct RealmStructureIDTag, uint8_t>;

struct BuiltinRealmStructure {
    Structure* (*findInitialStructure)(JSGlobalObject*) { nullptr };
    std::span<const BuiltinName> names { };
};

struct BuiltinOwnSlot {
    RealmStructureID structure { };
    uint8_t offset { 0 };
};

struct BuiltinImplementation {
    BuiltinImplementationKind kind { BuiltinImplementationKind::Generic };
    std::optional<BuiltinImplementationKind> otherwise { };
    BuiltinCallee callee { BuiltinCallee::None };
    std::optional<InlineBuiltin> inlineBuiltin { };
    std::array<uint8_t, 4> inlineParameters { };
    std::optional<uint8_t> maximumArgumentCount { };
    RuntimeFunctionID function { };
    BuiltinOwnSlot ownSlot { };
};

inline bool entersNativeCode(const BuiltinImplementation& implementation) { return implementation.callee != BuiltinCallee::Function; }

struct BuiltinCode {
    BuiltinImplementation implementation { };
    RuntimeFunctionID model { };
    std::optional<InlineBuiltin> fieldsForm { };
    std::optional<InlineBuiltin> prefixForm { };
    BuiltinRecordStretch<BuiltinResultProperty, BuiltinRecordPool::ResultProperties> prefixProperties { };
    std::optional<InlineBuiltin> listForm { };
    // Native code takes nothing but the bytes of these operands. The other overload does the same with a part of each: where the parts start and end, in bytes, are its last arguments.
    BuiltinOverloadID overloadInRanges { };
    std::array<std::optional<BuiltinOperand>, 3> operandsTakenAsBytes { };
};

struct BuiltinOverload {
    BuiltinID builtin { };
    BuiltinCallMode mode { BuiltinCallMode::Call };
    OptionSet<BuiltinOverloadFlag> flags { BuiltinOverloadFlag::IsUndeclared, BuiltinOverloadFlag::Throws };
    BuiltinRecordPointer<BuiltinParameter, BuiltinRecordPool::Parameters> receiver { };
    BuiltinRecordStretch<BuiltinParameter, BuiltinRecordPool::Parameters> parameters { };
    BuiltinResult result { };
    BuiltinEffects effects { };
    BuiltinFlow flow { };
    BuiltinCode code { };

    constexpr bool throws() const { return flags.contains(BuiltinOverloadFlag::Throws); }
    constexpr bool onlyPropagatesErrors() const { return flags.contains(BuiltinOverloadFlag::OnlyPropagatesErrors); }
    constexpr bool isUndeclared() const { return flags.contains(BuiltinOverloadFlag::IsUndeclared); }
    constexpr bool resultIsDeclaredOnly() const { return flags.contains(BuiltinOverloadFlag::ResultIsDeclaredOnly); }
    constexpr bool hasTypedResult() const { return result.typedResultForm != TypedResultForm::None; }

    constexpr RuntimeFunctionID libraryFunction() const
    {
        bool isCallOfLibraryFunction = code.implementation.kind == BuiltinImplementationKind::Compiled && !hasTypedResult();
        return isCallOfLibraryFunction ? code.implementation.function : RuntimeFunctionID();
    }

    constexpr RuntimeFunctionID slowPathFunction() const
    {
        return libraryFunction() ? RuntimeFunctionID() : code.implementation.function;
    }
};

struct BuiltinPackedOverload {
    BuiltinID builtin { };
    BuiltinCallMode mode { BuiltinCallMode::Call };
    OptionSet<BuiltinOverloadFlag> flags { };
    BuiltinRecordPointer<BuiltinParameter, BuiltinRecordPool::Parameters> receiver { };
    BuiltinRecordStretch<BuiltinParameter, BuiltinRecordPool::Parameters> parameters { };
    uint16_t result { 0 };
    uint16_t effects { 0 };
    uint16_t flow { 0 };
    uint16_t code { 0 };
};
static_assert(sizeof(BuiltinPackedOverload) == 18);

extern const BuiltinResult builtinTableResultsOfOverloads[];
extern const BuiltinEffects builtinTableEffectsOfOverloads[];
extern const BuiltinFlow builtinTableFlowsOfOverloads[];
extern const BuiltinCode builtinTableCodeOfOverloads[];

struct BuiltinDispatchTarget {
    BuiltinDispatchNodeID node { };
    BuiltinOverloadID overload { };

    constexpr bool isOverload() const { return !!overload; }
};

struct BuiltinDispatchBranch {
    BuiltinTypeID tested { };
    BuiltinDispatchTarget target { };
};

#define FOR_EACH_AOT_BUILTIN_RECORD_POOL(macro) \
    macro(Names, BuiltinName, builtinTableNamesOfOverloads) \
    macro(Parameters, BuiltinParameter, builtinTableParameters) \
    macro(Transfers, BuiltinTransfer, builtinTableTransfers) \
    macro(Callbacks, BuiltinCallback, builtinTableCallbacks) \
    macro(HandlerCalls, BuiltinHandlerCall, builtinTableHandlerCalls) \
    macro(PropertyActions, BuiltinPropertyAction, builtinTablePropertyActions) \
    macro(ResultProperties, BuiltinResultProperty, builtinTableResultProperties) \
    macro(SlotContents, BuiltinSlotContents, builtinTableSlotContents) \
    macro(TypeParts, BuiltinTypePart, builtinTableTypeParts) \
    macro(TypeLiterals, BuiltinName, builtinTableTypeLiterals) \
    macro(DispatchBranches, BuiltinDispatchBranch, builtinTableDispatchBranches) \
    macro(MemberClasses, BuiltinClassID, builtinTableMemberClasses) \
    macro(Places, BuiltinPlace, builtinTablePlaces)

#define AOT_DECLARE_BUILTIN_RECORD_POOL(pool, Record, array) \
    extern const Record array[]; \
    template<> struct BuiltinRecordsOfPool<BuiltinRecordPool::pool> { \
        static const Record* records() { return array; } \
    };
FOR_EACH_AOT_BUILTIN_RECORD_POOL(AOT_DECLARE_BUILTIN_RECORD_POOL)
#undef AOT_DECLARE_BUILTIN_RECORD_POOL

struct BuiltinDispatchNode {
    BuiltinOperand operand;
    BuiltinRecordStretch<BuiltinDispatchBranch, BuiltinRecordPool::DispatchBranches> branches { };
};

struct BuiltinDispatch {
    BuiltinID builtin { };
    BuiltinCallMode mode { BuiltinCallMode::Call };
    BuiltinDispatchTarget root { };
};

struct Builtin {
    BuiltinKind kind { BuiltinKind::Namespace };
    BuiltinOrigin origin { BuiltinOrigin::Engine };
    BuiltinScope scope { BuiltinScope::Realm };
    BuiltinMutability mutability { BuiltinMutability::ChangesOnItsOwn };
    BuiltinPrototypeLink prototypeLink { BuiltinPrototypeLink::Unknown };
    OptionSet<BuiltinPlatform> platforms { };
    OptionSet<BuiltinFlag> flags { };
    BuiltinID holder { };
    BuiltinName name { };
    BuiltinID inheritsFrom { };
    BuiltinClassID instanceClass { };
    BuiltinID sameObjectAs { };
    BuiltinID firstMember { };
    uint16_t memberCount { 0 };
    BuiltinOverloadID firstOverload { };
    uint16_t overloadCount { 0 };
    bool isNamedByText { false };
    bool isAddedAtFirstRead { false };
    bool isAddedAsEnumerable { false };
};

struct BuiltinSlot {
    BuiltinSlotID id { };
    BuiltinClassID owner { };
    BuiltinName name { };
    uint8_t indexInClass { 0 };
    std::optional<ContentsSlot> contentsSlot { };
    std::optional<uint8_t> indexInType { };
    AbstractHeap heap;
    BuiltinTypeID holds { };
};

struct BuiltinPropertyMaker {
    BuiltinName name;
    BuiltinID holder { };
    BuiltinName member;
};

struct BuiltinClass {
    BuiltinClassID id { };
    BuiltinOrigin origin { BuiltinOrigin::Engine };
    BuiltinClassID parent { };
    BuiltinID constructor { };
    BuiltinID prototype { };
    JSType type { CellType };
    BuiltinInstanceTest instanceTest { BuiltinInstanceTest::None };
    ArrayBufferSharingMode sharingMode { ArrayBufferSharingMode::Default };
    bool isPrimitiveWrapper { false };
    bool isNeverPristine { false };
    BuiltinPristineTest pristineTest { BuiltinPristineTest::Mark };
    bool hasClosureInstances { false };
    bool instancesAreEntries { false };
    bool definesCharactersOnNewInstances { false };
    bool listsNamesDefinedOnNewInstances { false };
    std::span<const BuiltinName> namesDefinedOnNewInstances { };
    std::span<const BuiltinResultProperty> typesOfNamesDefinedOnNewInstances { };
    std::span<const BuiltinPropertyMaker> makersOfNamesDefinedOnNewInstances { };
    std::span<const BuiltinName> namesReadToDescribeInstances { };
    bool constructorOnlyReadsPrototypeOfNewTarget { false };
    BuiltinName constructorAdder { };
    std::span<const BuiltinSlotID> slots { };
    std::optional<AbstractHeap> stateHeap { };
};

struct BuiltinLayoutField {
    BuiltinName name { };
    BuiltinTypeID type { };
    bool isOptional { false };
};

struct BuiltinLayout {
    BuiltinLayoutID id { };
    bool isTuple { false };
    std::span<const BuiltinLayoutField> fields { };
};

struct BuiltinCondition {
    BuiltinOperand operand { BuiltinOperand::receiver() };
    std::span<const BuiltinName> isOneOf { };
    std::span<const BuiltinName> startsWith { };
    BuiltinID calledOn { };
    BuiltinName hasProperty { };
    bool buildAdmitsRuntimeCode { false };
    bool callRunsOtherCode { false };
    bool objectNobodyKnowsIsWritten { false };
    bool testsReplaceBuiltins { false };
    BuiltinID programStores { };

    BuiltinName firstText() const { return !isOneOf.empty() ? isOneOf.front() : !startsWith.empty() ? startsWith.front() : BuiltinName(); }
    bool isAboutWhatProgramReplaces() const { return testsReplaceBuiltins || programStores; }
};

struct BuiltinFeatureUse {
    BuiltinID builtin { };
    BuiltinOverloadID overload { };
    RemovableFeature feature;
    bool isGuard { false };
    const BuiltinCondition* condition { nullptr };
};

struct BuiltinUnseenCode {
    BuiltinID builtin { };
    const BuiltinCondition* condition { nullptr };
};

struct BuiltinHostCall;

struct BuiltinHostUse {
    BuiltinHostReadUse use { BuiltinHostReadUse::Inspected };
    std::span<const BuiltinName> memberPath { };
    BuiltinID builtin { };
    uint8_t argumentIndex { 0 };
    BuiltinName nativeCell { };
    const BuiltinHostCall* call { nullptr };
};

struct BuiltinHostUses {
    bool areListed { false };
    std::span<const BuiltinHostUse> list { };
};

struct BuiltinHostCall {
    BuiltinCallbackTimes times { BuiltinCallbackTimes::Once };
    BuiltinPlace thisValue { };
    bool listsArguments { false };
    std::span<const BuiltinPlace> arguments { };
    BuiltinPlace otherArguments { };
    BuiltinHostUses result { };
};

struct BuiltinModuleLoad {
    BuiltinID builtin { };
    RuntimeModuleID module { };
    bool readsEveryExport { true };
    std::span<const BuiltinName> exports { };
    std::span<const BuiltinHostUses> usesOfExports { };
    const BuiltinCondition* condition { nullptr };
};

struct BuiltinCalledFunction {
    BuiltinID builtin { };
    RuntimeFunctionID function { };
    const BuiltinCondition* condition { nullptr };
    BuiltinHostUses uses { };
};

struct BuiltinHostObjectField {
    BuiltinName name { };
    BuiltinPlace value { };
};

struct BuiltinHostObjectInternalField {
    uint8_t index { 0 };
    BuiltinHostUses uses { };
};

struct BuiltinHostObject {
    BuiltinName nativeCell { };
    BuiltinID function { };
    BuiltinID prototype { };
    std::span<const BuiltinHostObjectField> fields { };
    std::span<const BuiltinHostObjectInternalField> internalFields { };
    bool keepsStringsOfStoredValues { false };
    // For each of these names the object has an accessor whose getter and setter are native code.
    std::span<const BuiltinName> nativeAccessors { };
    // The systems on which native code makes the object.
    OptionSet<BuiltinPlatform> platforms { everyBuiltinPlatform };
};

struct BuiltinNamedMemberCall {
    BuiltinName name { };
    std::span<const BuiltinPlace> arguments { };
};

struct BuiltinHostMethodCall {
    BuiltinName name { };
    BuiltinName nativeCell { };
    const BuiltinHostCall* call { nullptr };
};

inline constexpr size_t maxLiteralPartNames = 4;

struct BuiltinForms {
    BuiltinID builtin { };
    std::optional<InlineBuiltin> appendForm { };
    BuiltinID jsonTextForm { };
    std::optional<BuiltinOperand> jsonTextArgument { };
    BuiltinID literalPartsForm { };
    std::optional<BuiltinOperand> literalPartsArgument { };
    std::array<BuiltinName, maxLiteralPartNames> literalPartNames { };
    BuiltinID bodyBytesForm { };
};

struct BuiltinBoxedCall {
    BuiltinID builtin { };
    BuiltinBoxedStub stub;
};

struct BuiltinNumberConstant {
    BuiltinID builtin { };
    double value { 0 };
};

struct BuiltinStringConstant {
    BuiltinID builtin { };
    OptionSet<BuiltinPlatform> platforms { };
    OptionSet<BuiltinArchitecture> architectures { };
    BuiltinName text { };
};

struct BuiltinOverloadSummary {
    BuiltinID builtin { };
    BuiltinCallMode mode { BuiltinCallMode::Call };
    BuiltinImplementationKind kind { BuiltinImplementationKind::Generic };
    bool hasOtherwise { false };
    BuiltinCallee callee { BuiltinCallee::None };
    RuntimeFunctionID function { };
    BuiltinOwnSlot ownSlot { };
};

struct BuiltinSymbolConstant {
    BuiltinID symbol { };
    BuiltinName name { };
    WellKnownSymbol wellKnownSymbol;
};

struct BuiltinNameClasses {
    BuiltinName name { };
    BuiltinRecordStretch<BuiltinClassID, BuiltinRecordPool::MemberClasses> classes { };
};

struct BuiltinProtocolKeys {
    Protocol protocol;
    std::span<const BuiltinName> keys { };
};

struct BuiltinClosedKeys {
    BuiltinID holder { };
    std::span<const BuiltinName> names { };
    std::span<const BuiltinName> openNames { };
};

struct BuiltinNamesReadByNativeCode {
    BuiltinID holder { };
    std::span<const BuiltinName> names { };
    std::span<const BuiltinName> namesOnlyInspected { };
};

enum class BuiltinMark : uint8_t {
    IsAddedByOption,
    MayBeAbsent,
    EvaluatesTextInGlobalScope,
    HoldsLoader,
    IsSpy,
    IsNotEnumerable,
    IsReplacedBySetter,
    RunsWaitingJobs,
    IsCalledWithoutStartupLoads,
    InstancesHaveMethodCalledWithStartupLoads,
    LoadsCodeWithoutJSAccess,
    AnswersNullAsItsPrototype,
    ReadLoadsCodeByName,
    HoldsPathOfMainModule,
    IsResolverThatLoaderCalls,
    IsRequireThatLoaderCalls,
    InstancesArePrintedWithoutTheirGetters,
};

struct BuiltinMarkedEntry {
    BuiltinMark mark { };
    BuiltinID builtin { };
};

struct BuiltinLoaderOfMachineCode {
    BuiltinID callee { };
    BuiltinID named { };
};

struct RuntimeFunctionData {
    RuntimeFunctionID id { };
    RuntimeFunctionRole role { RuntimeFunctionRole::EngineBuiltin };
    OptionSet<RuntimeFunctionFlag> flags { };
    OptionSet<BuiltinPlatform> platforms { };
    std::span<const BuiltinID> implements { };
};

struct RuntimeModuleData {
    RuntimeModuleID id { };
    BuiltinID namespaceObject { };
    OptionSet<RuntimeModuleFlag> flags { };
};

struct BuiltinHostRead {
    BuiltinHostReadContext context { BuiltinHostReadContext::EntryPoint };
    BuiltinName exportName { };
    BuiltinHostReadUse use { BuiltinHostReadUse::Inspected };
    BuiltinID passedTo { };
    uint8_t argumentIndex { 0 };
};

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

template<size_t numberOfTexts, size_t numberOfCharacters>
struct BuiltinTextStorage {
    constexpr size_t size() const { return numberOfTexts; }
    constexpr ASCIILiteral operator[](size_t index) const { return ASCIILiteral::fromLiteralUnsafe(&characters[starts[index]]); }

    uint32_t count;
    uint32_t starts[numberOfTexts + 1];
    char characters[numberOfCharacters];
};

class BuiltinTexts {
public:
    constexpr BuiltinTexts() = default;

    template<size_t numberOfTexts, size_t numberOfCharacters>
    constexpr BuiltinTexts(const BuiltinTextStorage<numberOfTexts, numberOfCharacters>& storage LIFETIME_BOUND)
        : m_storage(&storage.count)
    {
    }

    size_t size() const { return m_storage ? *m_storage : 0; }

    std::span<const char> operator[](size_t index) const
    {
        size_t count = size();
        RELEASE_ASSERT(index < count);
        const uint32_t* starts = m_storage + 1;
        return { reinterpret_cast<const char*>(starts + count + 1) + starts[index], starts[index + 1] - starts[index] - 1 };
    }

private:
    const uint32_t* m_storage { nullptr };
};

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

struct BuiltinTableTokens {
    std::span<const BuiltinID> builtinsWithQualifiedTokens { };
    BuiltinTexts classes { };
    BuiltinTexts slots { };
    BuiltinTexts layouts { };
    BuiltinTexts modules { };
};

struct BuiltinTableNames {
    BuiltinTexts textNames { };
    BuiltinTexts symbolNames { };
    BuiltinTexts registeredSymbolNames { };
    std::span<const BuiltinSymbolConstant> symbolConstants { };
    std::span<const BuiltinNameClasses> classesByMemberName { };
    std::span<const BuiltinProtocolKeys> protocolKeys { };
    std::span<const uint32_t> textNameSlots { };
};

struct BuiltinTableAttachments {
    std::span<const BuiltinFeatureUse> featureUses { };
    std::span<const BuiltinUnseenCode> unseenCode { };
    std::span<const BuiltinModuleLoad> moduleLoads { };
    std::span<const BuiltinModuleLoad> startupLoads { };
    std::span<const BuiltinCalledFunction> calledFunctions { };
    std::span<const BuiltinCalledFunction> startupCalledFunctions { };
    std::span<const BuiltinHostObject> hostObjects { };
    std::span<const BuiltinNamedMemberCall> namedMemberCalls { };
    std::span<const BuiltinHostMethodCall> hostMethodCalls { };
    std::span<const BuiltinForms> forms { };
    std::span<const BuiltinBoxedCall> boxedCalls { };
    std::span<const BuiltinNumberConstant> numberConstants { };
    std::span<const BuiltinStringConstant> stringConstants { };
    std::span<const BuiltinOverloadSummary> overloadSummaries { };
};

struct BuiltinTableRuntime {
    std::span<const RuntimeModuleData> modules { };
    std::span<const RuntimeFunctionData> functions { };
    std::span<const RuntimeModuleID> startupModules { };
    std::span<const RuntimeFunctionID> functionsCalledWithStartupLoads { };
    std::span<const BuiltinName> namesOfHostMethodsCallingStartupLoads { };
    std::span<const RuntimeFunctionID> moduleLoaderFunctions { };
    std::span<const BuiltinHostRead> hostReads { };
    std::span<const BuiltinID> lockedObjects { };
    std::span<const BuiltinMarkedEntry> marks { };
    std::span<const BuiltinLoaderOfMachineCode> loadersOfMachineCode { };
    std::span<const BuiltinClosedKeys> closedKeys { };
    std::span<const BuiltinNamesReadByNativeCode> namesReadByNativeCode { };
    std::span<const BuiltinID> lockedMembers { };
    std::span<const BuiltinRealmStructure> realmStructures { };
};

struct BuiltinTableData {
    std::span<const Builtin> builtins { };
    std::span<const BuiltinPackedOverload> overloads { };
    std::span<const BuiltinDispatch> dispatches { };
    std::span<const BuiltinDispatchNode> dispatchNodes { };
    std::span<const BuiltinType> types { };
    std::span<const BuiltinClass> classes { };
    std::span<const BuiltinSlot> slots { };
    std::span<const BuiltinLayout> layouts { };
    BuiltinTableNames names { };
    BuiltinTableTokens tokens { };
    BuiltinTableAttachments attachments { };
    BuiltinTableRuntime runtime { };
};

extern const BuiltinTableData builtinTableData;

struct BuiltinClassTokenStorage;
extern const BuiltinPackedOverload builtinTableOverloads[];
extern const BuiltinDispatch builtinTableDispatches[];
extern const BuiltinDispatchNode builtinTableDispatchNodes[];
extern const BuiltinType builtinTableTypes[];
extern const BuiltinFeatureUse builtinTableFeatureUses[];
extern const BuiltinModuleLoad builtinTableModuleLoads[];
extern const BuiltinCalledFunction builtinTableCalledFunctions[];
extern const BuiltinCalledFunction builtinTableStartupCalledFunctions[];
extern const BuiltinHostObject builtinTableHostObjects[];
extern const BuiltinNamedMemberCall builtinTableNamedMemberCalls[];
extern const BuiltinHostMethodCall builtinTableHostMethodCalls[];
extern const BuiltinForms builtinTableForms[];
extern const BuiltinNameClasses builtinTableClassesByMemberName[];
extern const BuiltinClassTokenStorage builtinTableClassTokens;
// One per entry of runtimeFunctions(), in the same order. Runtime functions are only named in IR text.
extern const BuiltinTexts builtinTableRuntimeFunctionTokens;

struct BuiltinMemberLookup {
    enum class Outcome : uint8_t {
        Unknown,
        Absent,
        Found,
    };

    Outcome outcome { Outcome::Unknown };
    BuiltinID member { };
    BuiltinID holder { };
};

class BuiltinTable {
    WTF_MAKE_NONCOPYABLE(BuiltinTable);
public:
    constexpr explicit BuiltinTable(const BuiltinTableData& data)
        : m_data(data)
    {
    }

    JS_EXPORT_PRIVATE static const BuiltinTable& singleton();

    bool contains(BuiltinID id) const { return id.index() < m_data.builtins.size(); }
    bool contains(BuiltinOverloadID id) const { return id.index() < m_data.overloads.size(); }
    bool contains(BuiltinClassID id) const { return id.index() < m_data.classes.size(); }
    bool contains(BuiltinSlotID id) const { return id.index() < m_data.slots.size(); }
    bool contains(BuiltinLayoutID id) const { return id.index() < m_data.layouts.size(); }
    bool contains(BuiltinTypeID id) const { return id.index() < m_data.types.size(); }
    bool contains(BuiltinName name) const { return name.index() < nameCount(); }
    bool contains(RuntimeModuleID id) const { return id.index() < m_data.runtime.modules.size(); }
    bool contains(RealmStructureID id) const { return id.index() < m_data.runtime.realmStructures.size(); }
    bool contains(RuntimeFunctionID) const;

    unsigned builtinCount() const { return m_data.builtins.size(); }
    unsigned overloadCount() const { return m_data.overloads.size(); }
    unsigned classCount() const { return m_data.classes.size(); }
    unsigned slotCount() const { return m_data.slots.size(); }
    unsigned layoutCount() const { return m_data.layouts.size(); }
    unsigned nameCount() const { return m_data.names.textNames.size() + m_data.names.symbolNames.size() + m_data.names.registeredSymbolNames.size(); }
    unsigned moduleCount() const { return m_data.runtime.modules.size(); }

    JS_EXPORT_PRIVATE const Builtin& at(BuiltinID) const;
    const BuiltinOverload& at(BuiltinOverloadID) const;
    const BuiltinClass& at(BuiltinClassID) const;
    const BuiltinSlot& at(BuiltinSlotID) const;
    const BuiltinLayout& at(BuiltinLayoutID) const;
    const BuiltinType& at(BuiltinTypeID) const;
    const BuiltinDispatchNode& at(BuiltinDispatchNodeID) const;
    const RuntimeModuleData& at(RuntimeModuleID) const;
    const BuiltinRealmStructure& at(RealmStructureID) const;
    const RuntimeFunctionData& at(RuntimeFunctionID) const;

    std::span<const BuiltinOverload> overloadsOf(BuiltinID) const;
    BuiltinTableIDRange<BuiltinOverloadID> overloadIDsOf(BuiltinID) const;
    std::span<const BuiltinOverloadSummary> overloadSummariesOf(BuiltinID) const;
    const BuiltinOverloadSummary& summaryOf(BuiltinOverloadID) const;
    std::optional<BuiltinDispatchTarget> dispatchOf(BuiltinID, BuiltinCallMode) const;
    const BuiltinParameter* parameterFor(const BuiltinOverload&, BuiltinOperand position) const;

    JS_EXPORT_PRIVATE BuiltinTableIDRange<BuiltinID> membersOf(BuiltinID holder) const;
    JS_EXPORT_PRIVATE std::span<const Builtin> memberEntriesOf(BuiltinID holder) const;
    BuiltinID findGlobal(BuiltinName name) const { return findOwnMember(Builtins::GlobalThis, name); }
    BuiltinID findOwnMember(BuiltinID holder, BuiltinName) const;
    BuiltinMemberLookup findMember(BuiltinID holder, BuiltinName) const;
    BuiltinMemberLookup findForInstance(BuiltinClassID, BuiltinName) const;
    bool isRedefinedBelow(BuiltinClassID, BuiltinName) const;
    BuiltinID findOwnPropertyOfInstances(BuiltinClassID, BuiltinName) const;
    bool listsOwnPropertiesOfNewInstances(BuiltinClassID) const;
    bool mayDefineOnNewInstances(BuiltinClassID, BuiltinName) const;
    BuiltinTypeID typeOfNameDefinedOnNewInstances(BuiltinClassID, BuiltinName) const;
    bool definesCharactersOnNewInstances(BuiltinClassID) const;
    std::span<const BuiltinClassID> classesWithInstanceMember(BuiltinName) const;

    BuiltinClassID receiverClassOf(BuiltinID) const;
    bool isSameOrSubclassOf(BuiltinClassID, BuiltinClassID ancestor) const;
    TypedArrayType typedArrayTypeOf(BuiltinClassID) const;
    BuiltinClassID typedArrayClassOf(BuiltinClassID) const;
    BuiltinSlotID findSlot(BuiltinClassID, BuiltinName) const;
    BuiltinSlotID findSlot(BuiltinClassID, ContentsSlot) const;
    BuiltinSlotID slotInType(BuiltinClassID, unsigned indexInType) const;
    unsigned numberOfSlotsInType(BuiltinClassID) const;

    BuiltinID rootOf(BuiltinID) const;
    bool isUnder(BuiltinID member, BuiltinID holder) const;
    BuiltinID canonicalID(BuiltinID id) const { return at(id).sameObjectAs ? at(id).sameObjectAs : id; }
    BuiltinID namespaceOf(RuntimeModuleID id) const { return at(id).namespaceObject; }
    bool existsOn(BuiltinID id, BuiltinPlatform platform) const { return at(id).platforms.contains(platform); }
    bool isLocationOfLockedObject(BuiltinID) const;
    // A locked member that a test run may unlock: a function or an accessor under a name that neither the language nor a call reads by itself.
    JS_EXPORT_PRIVATE bool canBeReplacedInTestRuns(BuiltinID) const;
    // An object that takes no new names, and a text that a test run may open on it. No name: a text that the table does not have.
    JS_EXPORT_PRIVATE bool canTakeNameInTestRuns(BuiltinID holder, BuiltinName) const;
    JS_EXPORT_PRIVATE bool isHostFunctionOfEngine(BuiltinID) const;
    // Of the engine or of the host. A realm that locks its built-ins gives every native function one structure, which keeps the prototype.
    bool isNativeFunction(BuiltinID) const;
    JS_EXPORT_PRIVATE bool isFunctionOfEngine(BuiltinID) const;
    // `formatter.format`: the accessor answers a function that it has bound to the receiver, the same at every read. What a call of that function does stands among the overloads of the accessor.
    bool answersFunctionBoundToReceiver(BuiltinID member) const { return at(member).kind == BuiltinKind::Accessor && dispatchOf(member, BuiltinCallMode::Call); }
    bool hasLockedPrototypeLink(BuiltinID holder) const { return at(holder).flags.containsAny({ BuiltinFlag::IsLockedObject, BuiltinFlag::HasLockedMembers }) || isFunctionOfEngine(holder) || closesOnlyNamesOfCalls(holder); }
    bool closesOnlyNamesOfCalls(BuiltinID holder) const { return at(holder).kind == BuiltinKind::Constructor && !at(holder).flags.containsAny({ BuiltinFlag::IsLockedObject, BuiltinFlag::HasLockedMembers }) && !closedKeysOf(holder).empty(); }
    bool closesIndexKeys(BuiltinID holder) const { return at(holder).flags.contains(BuiltinFlag::IsLockedObject) || (at(holder).flags.contains(BuiltinFlag::HasLockedMembers) && at(holder).kind == BuiltinKind::Prototype); }
    JS_EXPORT_PRIVATE bool hasLockedPrototypeChain(BuiltinClassID) const;
    JS_EXPORT_PRIVATE bool mayHavePristineInstances(BuiltinClassID) const;
    std::span<const BuiltinName> closedKeysOf(BuiltinID holder) const;
    std::span<const BuiltinName> openNamesOf(BuiltinID holder) const;
    bool isClosedKey(BuiltinID holder, BuiltinName) const;
    JS_EXPORT_PRIVATE bool isClosedOnHostFunctions(BuiltinName) const;
    const BuiltinNamesReadByNativeCode* namesReadByNativeCode(BuiltinID holder) const;

    std::span<const BuiltinFeatureUse> featureUsesOf(BuiltinID) const;
    std::span<const BuiltinModuleLoad> moduleLoadsOf(BuiltinID) const;
    std::span<const BuiltinCalledFunction> calledFunctionsOf(BuiltinID) const;
    const BuiltinUnseenCode* unseenCodeOf(BuiltinID) const;
    bool letsInUnseenCode(BuiltinID) const;
    bool letsInUnseenCodeWhenCalled(BuiltinID) const;
    const BuiltinForms* formsOf(BuiltinID) const;
    std::optional<BuiltinBoxedStub> boxedCallStubOf(BuiltinID) const;
    std::span<const BuiltinBoxedCall> boxedCalls() const { return m_data.attachments.boxedCalls; }

    std::span<const BuiltinID> lockedObjects() const { return m_data.runtime.lockedObjects; }
    std::span<const BuiltinMarkedEntry> marked(BuiltinMark) const;
    bool has(BuiltinMark, BuiltinID) const;
    std::span<const BuiltinLoaderOfMachineCode> loadersOfMachineCode() const { return m_data.runtime.loadersOfMachineCode; }
    std::span<const BuiltinID> lockedMembers() const { return m_data.runtime.lockedMembers; }
    std::span<const RuntimeFunctionData> runtimeFunctions() const { return m_data.runtime.functions; }
    std::span<const RuntimeModuleID> startupModules() const { return m_data.runtime.startupModules; }
    std::span<const BuiltinModuleLoad> startupLoads() const { return m_data.attachments.startupLoads; }
    std::span<const RuntimeFunctionID> functionsCalledWithStartupLoads() const { return m_data.runtime.functionsCalledWithStartupLoads; }
    std::span<const BuiltinCalledFunction> startupCalledFunctions() const;
    std::span<const BuiltinHostObject> hostObjects() const;
    const BuiltinNamedMemberCall* callOfMemberNamed(BuiltinName) const;
    std::span<const BuiltinHostMethodCall> hostMethodCalls() const;
    std::span<const BuiltinName> namesOfHostMethodsCallingStartupLoads() const { return m_data.runtime.namesOfHostMethodsCallingStartupLoads; }
    std::span<const RuntimeFunctionID> moduleLoaderFunctions() const { return m_data.runtime.moduleLoaderFunctions; }
    std::span<const BuiltinHostRead> hostReads() const { return m_data.runtime.hostReads; }
    std::span<const BuiltinName> keysReadBy(Protocol) const;

    BuiltinName nameOf(std::span<const uint8_t> utf8) const;
    BuiltinName textNameOf(UniquedStringImpl* key) const;
    BuiltinName nameOf(VM&, UniquedStringImpl* key) const;
    BuiltinName nameOfSymbol(BuiltinID wellKnownSymbol) const;
    BuiltinName nameOf(WellKnownSymbol) const;
    std::optional<WellKnownSymbol> wellKnownSymbolOf(BuiltinName) const;
    bool isSymbolName(BuiltinName) const;
    bool isRegisteredSymbolName(BuiltinName) const;
    std::span<const uint8_t> textOf(BuiltinName) const;
    JS_EXPORT_PRIVATE String textOf(BuiltinID) const;
    JS_EXPORT_PRIVATE Identifier identifierOf(VM&, BuiltinName) const;
    std::optional<double> numberOf(BuiltinID) const;
    std::optional<StringView> stringOf(BuiltinID) const;
    BuiltinName nameOfString(BuiltinID stringConstant) const { return nameOfString(stringConstant, targetOfThisBuild()); }
    BuiltinName nameOfString(BuiltinID stringConstant, BuiltinTarget) const;

    String tokenOf(BuiltinID) const;
    StringView tokenOf(BuiltinClassID) const;
    StringView tokenOf(BuiltinSlotID) const;
    StringView tokenOf(BuiltinLayoutID) const;
    StringView tokenOf(RuntimeModuleID) const;
    String tokenOf(BuiltinOverloadID) const;
    BuiltinID builtinFromToken(StringView) const;
    BuiltinClassID classFromToken(StringView) const;
    BuiltinSlotID slotFromToken(StringView) const;
    BuiltinLayoutID layoutFromToken(StringView) const;
    RuntimeModuleID moduleFromToken(StringView) const;
    BuiltinOverloadID overloadFromToken(StringView) const;

    JS_EXPORT_PRIVATE Vector<String> mismatches(JSGlobalObject*) const;

    JS_EXPORT_PRIVATE JSValue describeForTesting(JSGlobalObject*) const;
    JS_EXPORT_PRIVATE JSValue describeFeatureUsesForTesting(JSGlobalObject*, BuiltinPlatform) const;

private:
    friend class BuiltinTableOfBuild;

    void appendComponent(StringBuilder&, BuiltinName) const;

    const BuiltinTableData& m_data;
};

// The table as one build sees it. In a test run the members that the tests replace are ConstantUnlessWritten, as Date.now is everywhere.
class BuiltinTableOfBuild {
    WTF_MAKE_NONCOPYABLE(BuiltinTableOfBuild);
public:
    explicit BuiltinTableOfBuild(const BuiltinTable&);

    const BuiltinTable& table() const LIFETIME_BOUND { return m_table; }
    // `members` ascend. Leaves out a locked member that canBeReplacedInTestRuns() refuses. `holdersOfNewNames`: the objects that get a name of their own opened, which the caller keeps.
    // `opensEveryHolder`: every object takes whatever name canTakeNameInTestRuns() admits.
    void unlock(std::span<const BuiltinID> members, std::span<const BuiltinID> holdersOfNewNames, bool opensEveryHolder = false);
    std::span<const BuiltinID> unlockedMembers() const LIFETIME_BOUND { return m_unlockedMembers.span(); }

private:
    void lockByNames(BuiltinID wholeObject);
    void openToNames(BuiltinID holder);

    BuiltinTableData m_data;
    Vector<Builtin> m_builtins;
    Vector<BuiltinID> m_lockedMembers;
    Vector<BuiltinID> m_lockedObjects;
    Vector<BuiltinClosedKeys> m_closedKeys;
    Vector<Vector<BuiltinName>> m_closedSymbols;
    Vector<BuiltinID> m_unlockedMembers;
    BuiltinTable m_table;
};

JSType toJSType(CheckedJSType);
LinkTimeConstant toLinkTimeConstant(LinkTimeConstantID);

bool isValidCheckedJSType(uint32_t, const BuiltinTable&);
bool isValidLinkTimeConstantID(uint32_t, const BuiltinTable&);

} // namespace AOT
} // namespace JSC

#endif // ENABLE(AOT)
