/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
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

enum class BuiltinKind : uint8_t {
    Namespace,
    Constructor,
    Function,
    Prototype,
    Method,
    Accessor,
    DataProperty,
    Constant,
    NativeBinding,
};

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

enum class BuiltinCallMode : uint8_t {
    Call,
    Construct,
    Get,
    Set,
};

enum class BuiltinOrigin : uint8_t {
    Engine,
    Host,
};

enum class BuiltinScope : uint8_t {
    Realm,
    Module,
};

enum class BuiltinMutability : uint8_t {
    ChangesOnItsOwn,
    ConstantUnlessWritten,
    AlwaysLocked,
};

enum class BuiltinPlatform : uint8_t {
    Darwin = 1 << 0,
    Linux = 1 << 1,
    Windows = 1 << 2,
    FreeBSD = 1 << 3,
    Android = 1 << 4,
};

inline constexpr OptionSet<BuiltinPlatform> everyBuiltinPlatform { BuiltinPlatform::Darwin, BuiltinPlatform::Linux, BuiltinPlatform::Windows, BuiltinPlatform::FreeBSD, BuiltinPlatform::Android };

enum class BuiltinArchitecture : uint8_t {
    ARM64 = 1 << 0,
    X86_64 = 1 << 1,
};

inline constexpr OptionSet<BuiltinArchitecture> everyBuiltinArchitecture { BuiltinArchitecture::ARM64, BuiltinArchitecture::X86_64 };

struct BuiltinTarget {
    BuiltinPlatform platform;
    BuiltinArchitecture architecture;

    friend constexpr bool operator==(BuiltinTarget, BuiltinTarget) = default;
};

// An executable is a copy of the Bun that builds it, so what a build is for is what the compiler itself runs on.
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

enum class BuiltinPrototypeLink : uint8_t {
    Unknown,
    Null,
    Builtin,
};

enum class BuiltinFlag : uint16_t {
    ListsAllMembers = 1 << 0,
    IsGlobalObject = 1 << 1,
    IsLockedObject = 1 << 2,
    RequiresHolderAsThis = 1 << 3,
    IsDeterministic = 1 << 4,
    HasPrivateAlias = 1 << 5,
    IsOwnPropertyOfInstances = 1 << 6,
    IsLazilyCreated = 1 << 7,
    StartsInstance = 1 << 8,
    LoadsModuleByName = 1 << 9,
    EvaluatesText = 1 << 10,
    RunsUnseenCode = 1 << 11,
    IsCompilerHelper = 1 << 12,
    IsDefaultExport = 1 << 13,
    HasLockedMembers = 1 << 14,
    IsClosedToOtherNames = 1 << 15,
};

enum class BuiltinOverloadFlag : uint8_t {
    IsUndeclared = 1 << 0,
    IsProvenByConstruction = 1 << 1,
    Throws = 1 << 2,
    RunsOnlyOperandCode = 1 << 3,
    ReadsStateBeforeConversions = 1 << 4,
    IsSpecialCase = 1 << 5,
    OnlyPropagatesErrors = 1 << 6,
    ResultIsDeclaredOnly = 1 << 7,
};

enum class BuiltinCapability : uint8_t {
    JS,
    Heap,
    None,
};

enum class BuiltinConversion : uint8_t {
    None,
    ToBoolean,
    ToNumber,
    ToNumeric,
    ToString,
    ToPropertyKey,
    ToObject,
    ToPrimitive,
    ToIntegerOrInfinity,
    ToLength,
    ToIndex,
    ToInt32,
    ToUint32,
    ToBigInt,
    Dictionary,
};

enum class BuiltinParameterMode : uint8_t {
    Required,
    Optional,
    Rest,
};

enum class BuiltinPrimitive : uint8_t {
    Undefined = 1 << 0,
    Null = 1 << 1,
    Boolean = 1 << 2,
    Number = 1 << 3,
    String = 1 << 4,
    Symbol = 1 << 5,
    BigInt = 1 << 6,
    NotPassed = 1 << 7,
};

enum class BuiltinTypeFlag : uint8_t {
    IsAnything = 1 << 0,
    HasAnyObject = 1 << 1,
    HasAnyFunction = 1 << 2,
    HasPlainData = 1 << 3,
    HasOnlyIntegers = 1 << 4,
};

enum class BuiltinFreshness : uint8_t {
    No,
    Unaliased,
    UnlessSpecies,
    Yes,
};

enum class BuiltinOperandRole : uint16_t {
    EvaluatedAsText = 1 << 0,
    ModuleSpecifier = 1 << 1,
    ReadAsCharacters = 1 << 2,
    Locale = 1 << 3,
    ContentKey = 1 << 4,
    ReplacesContents = 1 << 5,
    MembersAreOnlyInspected = 1 << 6,
    KeysAreNotListed = 1 << 7,
    MembersAreOfObjectsOnly = 1 << 8,
    FunctionsAreConvertedUnderNamesOnly = 1 << 9,
    BecomesLengthOfReceiver = 1 << 10,
    CopiesHolesToResult = 1 << 11,
};

enum class BuiltinTypePartKind : uint8_t {
    Instance,
    InstanceOfReceiverClass,
    TypedArrayOfReceiverElementType,
    Layout,
    Builtin,
    Operand,
    Slot,
    IteratedValues,
    CallbackResult,
    Prototype,
};

enum class BuiltinPlaceKind : uint8_t {
    None,
    Operand,
    Slot,
    IteratedValues,
    OwnValues,
    Result,
    ResultSlot,
    ResultField,
    NewValue,
    NativeMemory,
    Prototype,
    Member,
    NativeCell,
};

enum class BuiltinKeyKind : uint8_t {
    None,
    Any,
    Indexes,
    Names,
    Operand,
    ElementsOf,
};

enum class BuiltinCallbackTimes : uint8_t {
    Later,
    Repeatedly,
    PerElement,
    Once,
    PerEvent,
};

// Later is one call after the built-in has returned, PerEvent any number of them: once per request, message or event.
constexpr bool isCalledAfterReturn(BuiltinCallbackTimes times)
{
    return times == BuiltinCallbackTimes::Later || times == BuiltinCallbackTimes::PerEvent;
}

enum class BuiltinCallbackResultUse : uint8_t {
    Escapes,
    Ignored,
    Tested,
    Returned,
    Stored,
};

enum class BuiltinPropertyActionKind : uint8_t {
    ReadsByKey,
    StoresByKey,
    DeletesByKey,
    AssignsFrom,
    DefinesName,
    TakesDescriptor,
    TakesDescriptorMap,
    ChangesIntegrity,
    ChangesPrototype,
    BecomesPrototype,
    DefinesGetter,
    DefinesSetter,
    ReadsGetter,
    ReadsSetter,
    TestsByKey,
    DescribesOwnProperties,
    // The object is a map of descriptors of what the call makes and returns.
    DescribesResult,
    // Each own property of the value is defined on the object as it stands there, an accessor as an accessor.
    DefinesFrom,
};

enum class BuiltinIntegrityLevel : uint8_t {
    NonExtensible,
    Sealed,
    Frozen,
};

enum class BuiltinRangeRule : uint8_t {
    None,
    MinimumOfArguments,
    MaximumOfArguments,
    AbsoluteValue,
    IntegerPart,
    Sign,
};

enum class BuiltinImplementationKind : uint8_t {
    Generic,
    HostCall,
    // The compiler has a compiled function that a pass may call directly. What a CallBuiltin enters is `callee`.
    Compiled,
    Inline,
    Number,
    Cell,
    CachedGetter,
    OwnSlot,
};

enum class BuiltinCallee : uint8_t {
    None,
    Function,
    HostFunction,
    InternalFunction,
    // The native function behind a getter of a static table. It has no function object until somebody asks for a descriptor of the property, so it is entered by its address.
    CustomGetter,
};

enum class BuiltinInstanceTest : uint8_t {
    None,
    JSTypeEquals,
    ClassInfoInherits,
    SharingModeEquals,
};

// How compiled code tests that an instance has gained no property and kept its prototype. Mark: start-up marks the Structure of new instances.
enum class BuiltinPristineTest : uint8_t {
    Mark,
    ClosureAsCreated,
    InstanceStructure,
};

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

enum class BuiltinHostReadUse : uint8_t {
    // Native code looks at it and lets go of it.
    Inspected,
    Called,
    // It is an argument of a call of a built-in that native code makes.
    PassedTo,
    // It lies in a native cell from then on. The entries and records that name the cell say what becomes of it.
    Kept,
    // From then on an entry holds it: it is the function object of a function, what a property holds.
    IsValueOf,
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

// What the records of the table refer to stands in one array for each kind of record. A stretch of such an array is its first record and its length: four bytes where a span has sixteen.
enum class BuiltinRecordPool : uint8_t { Names, Parameters, Transfers, Callbacks, HandlerCalls, PropertyActions, ResultProperties, SlotContents, TypeParts, TypeLiterals, DispatchBranches, MemberClasses };

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
    // Of an Instance: of the class that is named. Without it the value may be of any class below that one.
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
    // ResultField that a record stores into: the field holds what the records of the overload store there and nothing else, whatever the type of the field allows.
    bool holdsOnlyWhatIsStored { false };
    BuiltinTypeID newValueType { };
    bool isSettled { false };
    // Member: the names that are read one after the other, starting at the operand. An absent name stands for every own value.
    std::span<const BuiltinName> memberPath { };
    // The steps of all the paths of the table are numbered in one sequence.
    uint16_t firstMemberStep { 0 };
    // NativeCell: memory of native code that JavaScript reaches only through the entries that name the cell.
    BuiltinName nativeCell { };
};

struct BuiltinKey {
    BuiltinKeyKind kind { BuiltinKeyKind::None };
    std::optional<BuiltinOperand> operand { };
    std::span<const BuiltinName> names { };
};

struct BuiltinTransfer {
    BuiltinPlace from { };
    BuiltinPlace to { };
};

struct BuiltinCallback {
    BuiltinOperand callee;
    BuiltinCallbackTimes times { BuiltinCallbackTimes::Later };
    bool constructs { false };
    // Without a place the constructed function is its own new.target.
    BuiltinPlace newTarget { };
    BuiltinPlace thisValue { };
    std::span<const BuiltinPlace> arguments { };
    BuiltinPlace spreadArguments { };
    BuiltinCallbackResultUse resultUse { BuiltinCallbackResultUse::Escapes };
    BuiltinPlace resultTo { };
};

// A function that is not itself an operand: a member of an options object, or what an earlier call left in a native cell.
struct BuiltinHandlerCall {
    BuiltinPlace callee { };
    BuiltinCallbackTimes times { BuiltinCallbackTimes::PerEvent };
    BuiltinPlace thisValue { };
    std::span<const BuiltinPlace> arguments { };
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
    // Of an overload that only propagates errors: inside this type the entry raises no error about the operand.
    BuiltinTypeID admitted { };
    OperandUses uses { OperandUses::all() };
    OptionSet<BuiltinOperandRole> roles { };
    Protocols protocols { Protocols::all() };
    BuiltinNameStretch namedMembers { };
    BuiltinNameStretch calledMembers { };
    // With FunctionsAreConvertedUnderNamesOnly: the names of namedMembers that the entry itself lists. The others end the path to a handler.
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
    TypedResultForm typedResultForm { TypedResultForm::None };
    std::optional<BuiltinOperand> typedResultSource { };
    std::optional<BuiltinOperand> iteratedOperand { };
    IterationKind iterationKind { IterationKind::Values };
    std::optional<BuiltinOperand> prototypeSource { };
    // Every own property that the new object has and an instance fresh from the constructor of its class has not.
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

// A Structure that a realm reaches: the one that a getter of JSGlobalObject answers, after one transition for each name, which adds the name.
struct BuiltinRealmStructure {
    Structure* (*findInitialStructure)(JSGlobalObject*) { nullptr };
    std::span<const BuiltinName> names { };
};

// An instance with this Structure holds the property at a known place: the offset counts the properties that the constructor defines ahead of it. Any other instance is read by name.
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

// A CallBuiltin that stays one enters the function object of the realm. False: that object is compiled code too, so no native frame lies between the caller and a callback.
inline bool entersNativeCode(const BuiltinImplementation& implementation) { return implementation.callee != BuiltinCallee::Function; }

struct BuiltinCode {
    BuiltinImplementation implementation { };
    RuntimeFunctionID model { };
    std::optional<InlineBuiltin> fieldsForm { };
    std::optional<InlineBuiltin> prefixForm { };
    BuiltinRecordStretch<BuiltinResultProperty, BuiltinRecordPool::ResultProperties> prefixProperties { };
    std::optional<InlineBuiltin> listForm { };
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
    // With operands that it admits the entry has no step of its own that throws.
    constexpr bool onlyPropagatesErrors() const { return flags.contains(BuiltinOverloadFlag::OnlyPropagatesErrors); }
    constexpr bool isUndeclared() const { return flags.contains(BuiltinOverloadFlag::IsUndeclared); }
    // Nothing proves the type of the result: native code may hand back any value. The type is what is expected, and compiled code tests the value where it arrives.
    constexpr bool resultIsDeclaredOnly() const { return flags.contains(BuiltinOverloadFlag::ResultIsDeclaredOnly); }
    constexpr bool hasTypedResult() const { return result.typedResultForm != TypedResultForm::None; }

    // The function of the library that a call is: its body says what the call does, and the call may enter it directly.
    constexpr RuntimeFunctionID libraryFunction() const
    {
        bool isCallOfLibraryFunction = code.implementation.kind == BuiltinImplementationKind::Compiled && !hasTypedResult();
        return isCallOfLibraryFunction ? code.implementation.function : RuntimeFunctionID();
    }

    // The function of the library that takes the calls no other code was made for. It is what the entry's own function object runs, and the records bound what it does.
    constexpr RuntimeFunctionID slowPathFunction() const
    {
        return libraryFunction() ? RuntimeFunctionID() : code.implementation.function;
    }
};

// The form in which the executable holds an overload.
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
    macro(MemberClasses, BuiltinClassID, builtinTableMemberClasses)

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
    // Of a native binding: some text of the library names it, and the host evaluates it by a number of its own: BuiltinBindings::hostNumberOf().
    bool isNamedByText { false };
};

// Every object leads to a built-in prototype, from there to a constructor, and from that to Function, which evaluates text. Native code follows the link only to find a species.
constexpr bool isConstructorLink(const Builtin& holder, const Builtin& member)
{
    return holder.kind == BuiltinKind::Prototype && member.kind == BuiltinKind::Constructor;
}

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

// Where the program has stored a function under `member` on `holder`, or above it, native code calls that function to make the property, with the instance among the operands, and puts there what it answers.
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
    TypedArrayType typedArrayType { NotTypedArray };
    BuiltinInstanceTest instanceTest { BuiltinInstanceTest::None };
    ArrayBufferSharingMode sharingMode { ArrayBufferSharingMode::Default };
    bool isPrimitiveWrapper { false };
    bool isNeverPristine { false };
    BuiltinPristineTest pristineTest { BuiltinPristineTest::Mark };
    bool hasClosureInstances { false };
    // Native code makes no instance of the class but the entries that inherit from its prototype object. An object nobody knows is an instance only where it is one of those entries.
    bool instancesAreEntries { false };
    // The constructor puts the characters of a string on an instance, each an own read-only property under its index.
    bool definesCharactersOnNewInstances { false };
    // Every own property that the constructor, or the engine later on its own, puts on an instance, beside those of the classes above. Ascending.
    bool listsNamesDefinedOnNewInstances { false };
    std::span<const BuiltinName> namesDefinedOnNewInstances { };
    // What native code puts under some of those names: primitives. A name that is not here may hold anything. Ascending by name.
    std::span<const BuiltinResultProperty> typesOfNamesDefinedOnNewInstances { };
    std::span<const BuiltinPropertyMaker> makersOfNamesDefinedOnNewInstances { };
    // The constructor reads `prototype` from new.target and nothing else, and it calls nothing that it finds on the new object, whatever code of its operands it runs.
    bool constructorOnlyReadsPrototypeOfNewTarget { false };
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

// About the text of an operand, about a property of an operand, about the build, about the overload that takes the call, or about the stores of the program.
struct BuiltinCondition {
    BuiltinOperand operand { BuiltinOperand::receiver() };
    std::span<const BuiltinName> isOneOf { };
    std::span<const BuiltinName> startsWith { };
    // With a text: and the receivers of the call list the object of this entry. A receiver nobody knows is not taken for it.
    BuiltinID calledOn { };
    // The operand may be an object that has a property of this name, its own or inherited. It holds for an object nobody knows.
    BuiltinName hasProperty { };
    bool buildAdmitsRuntimeCode { false };
    bool callRunsOtherCode { false };
    // The row speaks of a store into the object that the built-in answers: one that the table does not describe, or one of the host with keepsStringsOfStoredValues.
    bool objectNobodyKnowsIsWritten { false };

    // The first of the texts that it lists. None where it is not about the text of an operand.
    BuiltinName firstText() const { return !isOneOf.empty() ? isOneOf.front() : !startsWith.empty() ? startsWith.front() : BuiltinName(); }
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

// One thing that native code does with a value that it got hold of by itself: an export that it read from a module of the library, the object of a function of the library, what a call of its own answered.
struct BuiltinHostUse {
    BuiltinHostReadUse use { BuiltinHostReadUse::Inspected };
    // The names that native code reads one after the other, starting at the value, an element under its index. It uses what it finds at the end.
    std::span<const BuiltinName> memberPath { };
    // PassedTo: the built-in that is called. IsValueOf: the entry that holds the value.
    BuiltinID builtin { };
    uint8_t argumentIndex { 0 };
    BuiltinName nativeCell { };
    // Called. Without one nothing says what is passed, and anybody may get what the call answers.
    const BuiltinHostCall* call { nullptr };
};

struct BuiltinHostUses {
    // False: nothing says what native code does with the value, so anybody may get it.
    bool areListed { false };
    // Empty: native code drops the value.
    std::span<const BuiltinHostUse> list { };
};

// A call that native code makes where no call of a built-in is under way. Its places are None, NewValue, NativeCell and NativeMemory: there is no operand to name.
struct BuiltinHostCall {
    BuiltinCallbackTimes times { BuiltinCallbackTimes::Once };
    BuiltinPlace thisValue { };
    // False: native code passes what the annotations of the callee's own parameters say.
    bool listsArguments { false };
    std::span<const BuiltinPlace> arguments { };
    // What may follow the listed arguments, any number of times.
    BuiltinPlace otherArguments { };
    BuiltinHostUses result { };
};

struct BuiltinModuleLoad {
    BuiltinID builtin { };
    RuntimeModuleID module { };
    bool readsEveryExport { true };
    std::span<const BuiltinName> exports { };
    // One for each of the exports, in their order.
    std::span<const BuiltinHostUses> usesOfExports { };
    const BuiltinCondition* condition { nullptr };
};

struct BuiltinCalledFunction {
    BuiltinID builtin { };
    RuntimeFunctionID function { };
    const BuiltinCondition* condition { nullptr };
    // What native code does with the object of the function. Where nothing is listed it calls the function with what the annotations of the parameters say, and anybody may get what that answers.
    BuiltinHostUses uses { };
};

struct BuiltinHostObjectField {
    // None: every name and every index that native code may define besides those that other records give.
    BuiltinName name { };
    BuiltinPlace value { };
};

// Nothing says what native code puts there by itself: whoever reads the field may find anything beside what code of the library has stored.
struct BuiltinHostObjectInternalField {
    uint8_t index { 0 };
    // What native code does with what code of the library stores there.
    BuiltinHostUses uses { };
};

// An object that native code makes by itself and hands to code of the library or of the program. It lies in the native cell, and whoever passes it names the cell. One record stands for all the objects that native code makes there.
struct BuiltinHostObject {
    BuiltinName nativeCell { };
    // The entry of a function that native code makes, whose Call overloads say what a call of the object does. None: it is no function.
    BuiltinID function { };
    // None: null.
    BuiltinID prototype { };
    // The own properties that native code defines. A name has one record for each thing that may be put there.
    std::span<const BuiltinHostObjectField> fields { };
    // Empty: the object has no internal field.
    std::span<const BuiltinHostObjectInternalField> internalFields { };
    // Of whatever is stored or defined under any key the object keeps a string, which code that the stored value names may make.
    bool keepsStringsOfStoredValues { false };
};

// What native code passes to a member that it finds under the name on an operand and calls (Protocol::CallsNamed). The receiver is the object that has the member, and the protocol says what becomes of the answer.
struct BuiltinNamedMemberCall {
    BuiltinName name { };
    std::span<const BuiltinPlace> arguments { };
};

// Classes of the host, in the table or not, have a native method under the name (namesOfHostMethodsCallingStartupLoads), and a call of one calls what lies in the cell.
struct BuiltinHostMethodCall {
    BuiltinName name { };
    BuiltinName nativeCell { };
    const BuiltinHostCall* call { nullptr };
};

inline constexpr size_t maxLiteralPartNames = 4;

struct BuiltinForms {
    BuiltinID builtin { };
    std::optional<InlineBuiltin> appendForm { };
    bool hasHostAppendFunction { false };
    BuiltinID jsonTextForm { };
    std::optional<BuiltinOperand> jsonTextArgument { };
    // The function that does the same where the argument is an object literal that nobody else sees and that has no member but the names.
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

// What the entry holds on the targets that the record lists. An entry has one record for each text.
struct BuiltinStringConstant {
    BuiltinID builtin { };
    OptionSet<BuiltinPlatform> platforms { };
    OptionSet<BuiltinArchitecture> architectures { };
    // A name of the table, so every program has the text among its strings.
    BuiltinName text { };
};

// The part of an overload that the run time reads, at the position of the overload. The overloads themselves leave an executable together with the compiler.
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

// Native code that was not handed the object looks up these names on it and no other, and it stores to none. Ascending.
struct BuiltinNamesReadByNativeCode {
    BuiltinID holder { };
    std::span<const BuiltinName> names { };
    // Those of them whose value native code only tests, for being true or for being callable: it neither keeps nor calls it. A getter on the way runs all the same.
    std::span<const BuiltinName> namesOnlyInspected { };
};

// What code of the engine asks about an entry, and few entries have. The table lists the entries of each mark, so the engine names none of them.
enum class BuiltinMark : uint8_t {
    // A flag of the command line or an option of the engine puts it on the global object or on process.
    IsAddedByOption,
    // Compiles text: a script or a function in the global scope of this realm, or code in a realm of its own, which has of this one what it is handed.
    EvaluatesTextInGlobalScope,
    // Who reads it has a loader of modules in hand.
    HoldsLoader,
    // Replaces a member of its first operand by a function of its own.
    IsSpy,
    // Whoever goes over the enumerable properties of its holder passes it by: Object.keys, values, entries and assign, a spread, JSON.stringify.
    IsNotEnumerable,
};

struct BuiltinMarkedEntry {
    BuiltinMark mark { };
    BuiltinID builtin { };
};

// A call of `callee` loads machine code that nobody has seen. A report names `named`, which is what a program writes.
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

// Texts one behind the other, each with a zero behind it, and the place where each starts.
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

    // A text has a zero behind it and lasts as long as the program.
    ASCIILiteral literalAt(size_t index) const { return ASCIILiteral::fromLiteralUnsafe((*this)[index].data()); }

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

// The arrays that leave an executable together with the compiler. Their readers name them: see keptByThisReader() in AOTBuiltinTable.cpp.
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
    unsigned realmStructureCount() const { return m_data.runtime.realmStructures.size(); }

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
    // One for each overload, in the order of overloadsOf(). Code that stays in an executable without the compiler asks here.
    std::span<const BuiltinOverloadSummary> overloadSummariesOf(BuiltinID) const;
    const BuiltinOverloadSummary& summaryOf(BuiltinOverloadID) const;
    std::optional<BuiltinDispatchTarget> dispatchOf(BuiltinID, BuiltinCallMode) const;
    const BuiltinParameter* parameterFor(const BuiltinOverload&, BuiltinOperand position) const;

    BuiltinTableIDRange<BuiltinID> membersOf(BuiltinID holder) const;
    BuiltinID findGlobal(BuiltinName name) const { return findOwnMember(Builtins::GlobalThis, name); }
    BuiltinID findOwnMember(BuiltinID holder, BuiltinName) const;
    BuiltinMemberLookup findMember(BuiltinID holder, BuiltinName) const;
    BuiltinMemberLookup findForInstance(BuiltinClassID, BuiltinName) const;
    // A value of a class may be an instance of a class below it and passes the tests of both. Whether the prototype object of such a class has a member of its own under the name.
    bool isRedefinedBelow(BuiltinClassID, BuiltinName) const;
    BuiltinID findOwnPropertyOfInstances(BuiltinClassID, BuiltinName) const;
    // Whether a new instance has no own property but the ones the table marks. An unaliased result has own properties that the table does not declare.
    bool listsOwnPropertiesOfNewInstances(BuiltinClassID) const;
    // Whether an instance that only the constructor and the engine have touched may have an own property that the class does not declare. An empty name stands for a key the table has no name for.
    bool mayDefineOnNewInstances(BuiltinClassID, BuiltinName) const;
    // None: the class that lists the name does not say what native code puts there.
    BuiltinTypeID typeOfNameDefinedOnNewInstances(BuiltinClassID declaringClass, BuiltinName) const;
    // Whether the class or one above it has the column. No slot stands for these properties.
    bool definesCharactersOnNewInstances(BuiltinClassID) const;
    std::span<const BuiltinClassID> classesWithInstanceMember(BuiltinName) const;

    BuiltinClassID receiverClassOf(BuiltinID) const;
    bool isSameOrSubclassOf(BuiltinClassID, BuiltinClassID ancestor) const;
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
    // A function of the engine whose object in the realm is a host function.
    JS_EXPORT_PRIVATE bool isHostFunctionOfEngine(BuiltinID) const;
    // Such a function, or one that the library writes in JavaScript: its closure takes none of the four names and no other prototype either (Closure::refusesOwnProperty, Closure::setPrototype).
    JS_EXPORT_PRIVATE bool isFunctionOfEngine(BuiltinID) const;
    bool hasLockedPrototypeLink(BuiltinID holder) const { return at(holder).flags.containsAny({ BuiltinFlag::IsLockedObject, BuiltinFlag::HasLockedMembers }) || isFunctionOfEngine(holder) || closesOnlyNamesOfCalls(holder); }
    // A constructor of the engine without a lock of its own. It takes none of the four names and no other prototype, like a function of the engine, and whatever else a program puts on it.
    bool closesOnlyNamesOfCalls(BuiltinID holder) const { return at(holder).kind == BuiltinKind::Constructor && !at(holder).flags.containsAny({ BuiltinFlag::IsLockedObject, BuiltinFlag::HasLockedMembers }) && !closedKeysOf(holder).empty(); }
    // An element that an instance lacks is looked for in its prototype objects. Nothing relies on what a constructor or a namespace object lacks under an index, so whoever likes adds one.
    bool closesIndexKeys(BuiltinID holder) const { return at(holder).flags.contains(BuiltinFlag::IsLockedObject) || (at(holder).flags.contains(BuiltinFlag::HasLockedMembers) && at(holder).kind == BuiltinKind::Prototype); }
    // Every object from the prototype object of the class up to null keeps its prototype link.
    JS_EXPORT_PRIVATE bool hasLockedPrototypeChain(BuiltinClassID) const;
    // Start-up marks the Structure that the realm gives a new instance (forEachInstanceStructure), and from then on a cell that gets another prototype or a name that shadows a member loses the mark.
    JS_EXPORT_PRIVATE bool mayHavePristineInstances(BuiltinClassID) const;
    std::span<const BuiltinName> closedKeysOf(BuiltinID holder) const;
    std::span<const BuiltinName> openNamesOf(BuiltinID holder) const;
    bool isClosedKey(BuiltinID holder, BuiltinName) const;
    // The names that no function of the engine has of its own.
    JS_EXPORT_PRIVATE bool isClosedOnHostFunctions(BuiltinName) const;
    // Null: nothing says what native code looks up on the object, so it finds whatever a program puts there.
    const BuiltinNamesReadByNativeCode* namesReadByNativeCode(BuiltinID holder) const;

    std::span<const BuiltinFeatureUse> featureUsesOf(BuiltinID) const;
    std::span<const BuiltinModuleLoad> moduleLoadsOf(BuiltinID) const;
    std::span<const BuiltinCalledFunction> calledFunctionsOf(BuiltinID) const;
    const BuiltinUnseenCode* unseenCodeOf(BuiltinID) const;
    bool letsInUnseenCode(BuiltinID) const;
    const BuiltinForms* formsOf(BuiltinID) const;
    std::optional<BuiltinBoxedStub> boxedCallStubOf(BuiltinID) const;
    std::span<const BuiltinBoxedCall> boxedCalls() const { return m_data.attachments.boxedCalls; }

    std::span<const BuiltinID> lockedObjects() const { return m_data.runtime.lockedObjects; }
    // Ascending.
    std::span<const BuiltinMarkedEntry> marked(BuiltinMark) const;
    bool has(BuiltinMark, BuiltinID) const;
    std::span<const BuiltinLoaderOfMachineCode> loadersOfMachineCode() const { return m_data.runtime.loadersOfMachineCode; }
    std::span<const BuiltinID> lockedMembers() const { return m_data.runtime.lockedMembers; }
    std::span<const RuntimeFunctionData> runtimeFunctions() const { return m_data.runtime.functions; }
    std::span<const RuntimeModuleID> startupModules() const { return m_data.runtime.startupModules; }
    // What native code reads from each start-up module. These records name no built-in.
    std::span<const BuiltinModuleLoad> startupLoads() const { return m_data.attachments.startupLoads; }
    // Native code calls these only where it hands on what it reads from the start-up modules.
    std::span<const RuntimeFunctionID> functionsCalledWithStartupLoads() const { return m_data.runtime.functionsCalledWithStartupLoads; }
    // The same functions in the same order, each with what native code does with its object. These records name no built-in.
    std::span<const BuiltinCalledFunction> startupCalledFunctions() const;
    // The position of a record in the list tells its object from the others.
    std::span<const BuiltinHostObject> hostObjects() const;
    // Null: nothing says what native code passes.
    const BuiltinNamedMemberCall* callOfMemberNamed(BuiltinName) const;
    std::span<const BuiltinHostMethodCall> hostMethodCalls() const;
    // Classes of the host, in the table or not, have a native method under each of these names that calls what native code reads from the start-up modules.
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
    Identifier identifierOf(VM&, BuiltinName) const;
    std::optional<double> numberOf(BuiltinID) const;
    // The text that the target decides: what the entry holds in the executable that this build makes.
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
    void appendComponent(StringBuilder&, BuiltinName) const;

    const BuiltinTableData& m_data;
};

JSType toJSType(CheckedJSType);
LinkTimeConstant toLinkTimeConstant(LinkTimeConstantID);

bool isValidCheckedJSType(uint32_t, const BuiltinTable&);
bool isValidLinkTimeConstantID(uint32_t, const BuiltinTable&);

} // namespace AOT
} // namespace JSC

#endif // ENABLE(AOT)
