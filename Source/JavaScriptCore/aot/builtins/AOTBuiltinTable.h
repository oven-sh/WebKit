/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTSchemaEnums.h"
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
#include <wtf/Vector.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/StringView.h>
#include <wtf/text/WTFString.h>

namespace JSC {

class Identifier;
class JSGlobalObject;
class JSValue;
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
    Compiled,
    Inline,
    Number,
    Cell,
    CachedGetter,
};

enum class BuiltinCallee : uint8_t {
    None,
    Function,
    HostFunction,
    InternalFunction,
};

enum class BuiltinInstanceTest : uint8_t {
    None,
    JSTypeEquals,
    ClassInfoInherits,
    SharingModeEquals,
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
    Inspected,
    Called,
    PassedTo,
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

struct BuiltinTypePart {
    BuiltinTypePartKind kind { BuiltinTypePartKind::Instance };
    PristineAspectSet pristine { };
    std::optional<BuiltinOperand> operand { };
    BuiltinClassID builtinClass { };
    BuiltinLayoutID layout { };
    BuiltinID builtin { };
    BuiltinSlotID slot { };
    std::span<const BuiltinSlotContents> contents { };
    bool isSettled { false };
};

struct BuiltinType {
    OptionSet<BuiltinTypeFlag> flags { BuiltinTypeFlag::IsAnything };
    OptionSet<BuiltinPrimitive> primitives { };
    BuiltinFreshness freshness { BuiltinFreshness::No };
    BuiltinIntegerRange integers { };
    std::span<const BuiltinName> stringLiterals { };
    std::span<const BuiltinTypePart> parts { };

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
    OperandUses uses { OperandUses::all() };
    OptionSet<BuiltinOperandRole> roles { };
    Protocols protocols { Protocols::all() };
    std::span<const BuiltinName> namedMembers { };
    std::span<const BuiltinName> calledMembers { };
    // With FunctionsAreConvertedUnderNamesOnly: the names of namedMembers that the entry itself lists. The others end the path to a handler.
    std::span<const BuiltinName> convertedMembers { };
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
};

struct BuiltinEffects {
    BuiltinCapability capability { BuiltinCapability::JS };
    AbstractHeaps reads { };
    AbstractHeaps writes { };
};

struct BuiltinFlow {
    std::span<const BuiltinTransfer> transfers { };
    std::span<const BuiltinCallback> callbacks { };
    std::span<const BuiltinPropertyAction> propertyActions { };
    std::span<const BuiltinHandlerCall> handlerCalls { };
};

struct BuiltinImplementation {
    BuiltinImplementationKind kind { BuiltinImplementationKind::Generic };
    std::optional<BuiltinImplementationKind> otherwise { };
    BuiltinCallee callee { BuiltinCallee::None };
    std::optional<InlineBuiltin> inlineBuiltin { };
    std::array<uint8_t, 4> inlineParameters { };
    std::optional<uint8_t> maximumArgumentCount { };
    RuntimeFunctionID function { };
};

struct BuiltinResultProperty {
    BuiltinName name;
    BuiltinTypeID type { };
};

struct BuiltinCode {
    BuiltinImplementation implementation { };
    RuntimeFunctionID model { };
    std::optional<InlineBuiltin> fieldsForm { };
    std::optional<InlineBuiltin> prefixForm { };
    std::span<const BuiltinResultProperty> prefixProperties { };
    std::optional<InlineBuiltin> listForm { };
};

struct BuiltinOverload {
    BuiltinID builtin { };
    BuiltinCallMode mode { BuiltinCallMode::Call };
    OptionSet<BuiltinOverloadFlag> flags { BuiltinOverloadFlag::IsUndeclared, BuiltinOverloadFlag::Throws };
    const BuiltinParameter* receiver { nullptr };
    std::span<const BuiltinParameter> parameters { };
    BuiltinResult result { };
    BuiltinEffects effects { };
    BuiltinFlow flow { };
    BuiltinCode code { };
    uint64_t textHash { 0 };

    constexpr bool throws() const { return flags.contains(BuiltinOverloadFlag::Throws); }
    constexpr bool isUndeclared() const { return flags.contains(BuiltinOverloadFlag::IsUndeclared); }
    constexpr bool hasTypedResult() const { return result.typedResultForm != TypedResultForm::None; }

    // The function of the library that a call is: its body says what the call does, and the call may enter it directly.
    // A call with a typed result runs code derived from what the result is expected to be, which only a CallBuiltin carries. The records say what that code does.
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

struct BuiltinDispatchTarget {
    BuiltinDispatchNodeID node { };
    BuiltinOverloadID overload { };

    constexpr bool isOverload() const { return !!overload; }
};

struct BuiltinDispatchBranch {
    BuiltinTypeID tested { };
    BuiltinDispatchTarget target { };
};

struct BuiltinDispatchNode {
    BuiltinOperand operand;
    std::span<const BuiltinDispatchBranch> branches { };
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
    uint16_t hostNumber { 0 };
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
    bool hasClosureInstances { false };
    // The constructor puts the characters of a string on an instance, each an own read-only property under its index.
    bool definesCharactersOnNewInstances { false };
    // Every own property that the constructor, or the engine later on its own, puts on an instance, beside those of the classes above. Ascending.
    bool listsNamesDefinedOnNewInstances { false };
    std::span<const BuiltinName> namesDefinedOnNewInstances { };
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

// About the text of an operand, about the build, or about the overload that takes the call.
struct BuiltinCondition {
    BuiltinOperand operand { BuiltinOperand::receiver() };
    std::span<const BuiltinName> isOneOf { };
    std::span<const BuiltinName> startsWith { };
    bool buildAdmitsRuntimeCode { false };
    bool callRunsOtherCode { false };
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

struct BuiltinModuleLoad {
    BuiltinID builtin { };
    RuntimeModuleID module { };
    bool readsEveryExport { true };
    std::span<const BuiltinName> exports { };
    const BuiltinCondition* condition { nullptr };
};

struct BuiltinCalledFunction {
    BuiltinID builtin { };
    RuntimeFunctionID function { };
    const BuiltinCondition* condition { nullptr };
};

struct BuiltinForms {
    BuiltinID builtin { };
    std::optional<InlineBuiltin> appendForm { };
    bool hasHostAppendFunction { false };
    BuiltinID jsonTextForm { };
    std::optional<BuiltinOperand> jsonTextArgument { };
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

struct BuiltinSymbolConstant {
    BuiltinID symbol { };
    BuiltinName name { };
    WellKnownSymbol wellKnownSymbol;
};

struct BuiltinNameClasses {
    BuiltinName name { };
    std::span<const BuiltinClassID> classes { };
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

struct BuiltinTableTokens {
    std::span<const ASCIILiteral> builtins { };
    std::span<const ASCIILiteral> classes { };
    std::span<const ASCIILiteral> slots { };
    std::span<const ASCIILiteral> layouts { };
    std::span<const ASCIILiteral> modules { };
};

struct BuiltinTableNames {
    std::span<const ASCIILiteral> textNames { };
    std::span<const ASCIILiteral> symbolNames { };
    std::span<const ASCIILiteral> registeredSymbolNames { };
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
    std::span<const BuiltinForms> forms { };
    std::span<const BuiltinBoxedCall> boxedCalls { };
    std::span<const BuiltinNumberConstant> numberConstants { };
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
    std::span<const BuiltinClosedKeys> closedKeys { };
    std::span<const BuiltinID> lockedMembers { };
};

struct BuiltinTableData {
    std::span<const Builtin> builtins { };
    std::span<const BuiltinOverload> overloads { };
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
    const RuntimeFunctionData& at(RuntimeFunctionID) const;

    std::span<const BuiltinOverload> overloadsOf(BuiltinID) const;
    BuiltinTableIDRange<BuiltinOverloadID> overloadIDsOf(BuiltinID) const;
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
    bool hasLockedPrototypeLink(BuiltinID holder) const { return at(holder).flags.containsAny({ BuiltinFlag::IsLockedObject, BuiltinFlag::HasLockedMembers }); }
    bool closesIndexKeys(BuiltinID holder) const { return hasLockedPrototypeLink(holder); }
    // Every object from the prototype object of the class up to null keeps its prototype link.
    JS_EXPORT_PRIVATE bool hasLockedPrototypeChain(BuiltinClassID) const;
    // Start-up marks the Structure that the realm gives a new instance (forEachInstanceStructure), and from then on a cell that gets another prototype or a name that shadows a member loses the mark.
    JS_EXPORT_PRIVATE bool mayHavePristineInstances(BuiltinClassID) const;
    std::span<const BuiltinName> closedKeysOf(BuiltinID holder) const;
    std::span<const BuiltinName> openNamesOf(BuiltinID holder) const;
    bool isClosedKey(BuiltinID holder, BuiltinName) const;

    std::span<const BuiltinFeatureUse> featureUsesOf(BuiltinID) const;
    std::span<const BuiltinModuleLoad> moduleLoadsOf(BuiltinID) const;
    std::span<const BuiltinCalledFunction> calledFunctionsOf(BuiltinID) const;
    const BuiltinUnseenCode* unseenCodeOf(BuiltinID) const;
    bool letsInUnseenCode(BuiltinID) const;
    const BuiltinForms* formsOf(BuiltinID) const;
    std::optional<BuiltinBoxedStub> boxedCallStubOf(BuiltinID) const;
    std::span<const BuiltinBoxedCall> boxedCalls() const { return m_data.attachments.boxedCalls; }

    std::span<const BuiltinID> lockedObjects() const { return m_data.runtime.lockedObjects; }
    std::span<const BuiltinID> lockedMembers() const { return m_data.runtime.lockedMembers; }
    std::span<const RuntimeFunctionData> runtimeFunctions() const { return m_data.runtime.functions; }
    std::span<const RuntimeModuleID> startupModules() const { return m_data.runtime.startupModules; }
    // What native code reads from each start-up module. These records name no built-in.
    std::span<const BuiltinModuleLoad> startupLoads() const { return m_data.attachments.startupLoads; }
    // Native code calls these only where it hands on what it reads from the start-up modules.
    std::span<const RuntimeFunctionID> functionsCalledWithStartupLoads() const { return m_data.runtime.functionsCalledWithStartupLoads; }
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

    StringView tokenOf(BuiltinID) const;
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
    const BuiltinTableData& m_data;
};

JSType toJSType(CheckedJSType);
LinkTimeConstant toLinkTimeConstant(LinkTimeConstantID);

bool isValidCheckedJSType(uint32_t, const BuiltinTable&);
bool isValidLinkTimeConstantID(uint32_t, const BuiltinTable&);

} // namespace AOT
} // namespace JSC

#endif // ENABLE(AOT)
