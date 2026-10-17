/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed { namespace Format {

static constexpr uint32_t magic = 0x52495442;
static constexpr uint32_t version = 37;
static constexpr uint32_t none = UINT32_MAX;

struct Section {
    uint32_t offsetInBytes;
    uint32_t count;
};

struct Span {
    uint32_t start;
    uint32_t count;
};

struct String {
    uint32_t offset;
    uint32_t length;
};

enum Primitive : uint32_t {
    PrimitiveUndefined = 1 << 0,
    PrimitiveNull = 1 << 1,
    PrimitiveFalse = 1 << 2,
    PrimitiveTrue = 1 << 3,
    PrimitiveNumber = 1 << 4,
    PrimitiveString = 1 << 5,
    PrimitiveSymbol = 1 << 6,
    PrimitiveBigInt = 1 << 7,
    PrimitiveLiteralString = 1 << 8,
};

enum DeclaredTypeFlag : uint32_t {
    DeclaredAnyObject = 1 << 0,
    DeclaredAnyFunction = 1 << 1,
    DeclaredIntegers = 1 << 2,
};

struct DeclaredType {
    uint32_t primitives;
    uint32_t flags;
    uint32_t minIntegerLow;
    uint32_t minIntegerHigh;
    uint32_t maxIntegerLow;
    uint32_t maxIntegerHigh;
    Span objects;
};

enum class ObjectPartKind : uint32_t {
    Class,
    GlobalClass,
    Array,
};

struct ObjectPart {
    ObjectPartKind kind;
    uint32_t shapeOrName;
    Span typeArguments;
};

enum class ShapeKind : uint32_t {
    Record,
    Instance,
    Environment,
};

enum FieldFlag : uint32_t {
    FieldIsOptional = 1 << 0,
    FieldIsPredictedInt32 = 1 << 1,
    FieldIsImmutable = 1 << 2,
    FieldIsMethod = 1 << 3,
    FieldIsAnnotated = 1 << 4,
    FieldIsNeverReadUninitialized = 1 << 5,
    FieldIsStoredOnce = 1 << 6,
    FieldIsNeverStored = 1 << 7,
    FieldIsSetAtCreation = 1 << 8,
    FieldMayBeSetAtCreation = 1 << 9,
    FieldIsAbsentAtCreation = 1 << 10,
    FieldIsNeverObservedAbsent = 1 << 11,
    FieldDisallowsUndefined = 1 << 12,
};

enum SingleFunctionFlag : uint32_t {
    SingleFunctionIsProven = 1 << 0,
    SingleFunctionOrNullish = 1 << 2,
};

enum class LiteralKind : uint32_t {
    String,
    Number,
};

struct Literal {
    LiteralKind kind;
    uint32_t low;
    uint32_t high;
};

struct Field {
    uint32_t name;
    uint32_t declaredType;
    uint32_t flags;
    Span literals;
    uint32_t typeSource;
    uint32_t singleFunction;
    uint32_t singleFunctionFlags;
};

enum class MethodKind : uint32_t {
    Method,
    Getter,
    Setter,
};

enum MethodFlag : uint32_t {
    MethodIsStatic = 1 << 0,
    MethodKeyIsSymbol = 1 << 1,
    MethodMayBeReplaced = 1 << 2,
};

struct Method {
    uint32_t name;
    uint32_t function;
    MethodKind kind;
    uint32_t flags;
    uint32_t key;
};

enum ShapeFlag : uint32_t {
    ShapeIsAllocated = 1 << 0,
    ShapeMayBeFrozen = 1 << 1,
    ShapeHasFixedLayout = 1 << 2,
    ShapeIsEvaluatedOnce = 1 << 3,
    ShapeHasUntypedBase = 1 << 4,
    ShapeHasFixedMembers = 1 << 5,
    ShapeMayHaveOtherPrototype = 1 << 6,
    ShapeAcceptsUntypedObjects = 1 << 7,
    ShapeHasComputedStaticMemberName = 1 << 8,
    ShapeConstructorMayBeReplaced = 1 << 9,
    ShapeMayBeReused = 1 << 10,
    ShapeIsRelaxedForLogging = 1 << 11,
    ShapeAcceptsOnlyOtherLayouts = 1 << 12,
};

struct Shape {
    ShapeKind kind;
    uint32_t name;
    uint32_t base;
    Span fields;
    Span methods;
    uint32_t constructor;
    uint32_t flags;
    Span members;
    Span structuralSubtypes;
    Span specializations;
    uint32_t environment;
    Span staticFields;
    uint32_t declaredPath;
    uint32_t declaredLine;
};

enum class Initialization : uint32_t {
    Uninitialized,
    Undefined,
    NeverReadUninitialized,
    NeverReadUninitializedStoredOnce,
    Zero,
};

struct Variable {
    uint32_t name;
    uint32_t module;
    uint32_t declaredType;
    Initialization initialization;
    uint32_t singleFunction;
    uint32_t singleFunctionFlags;
};

struct Module {
    uint32_t path;
    uint32_t initializer;
    uint32_t displayPath;
    Span lineTable;
    Span printed;
};

enum class FunctionKind : uint32_t {
    Plain,
    Arrow,
    Method,
    Constructor,
    Module,
    Bound,
};

enum FunctionFlag : uint32_t {
    FunctionIsAsync = 1 << 0,
    FunctionIsGenerator = 1 << 1,
    FunctionIsUsedAsValue = 1 << 2,
    FunctionHasRestParameter = 1 << 3,
    FunctionHasThis = 1 << 4,
    FunctionStartsEagerly = 1 << 5,
    FunctionIsTyped = 1 << 6,
    FunctionHasNewTarget = 1 << 7,
    FunctionIsBuiltin = 1 << 8,
    FunctionIsInBuiltinModule = 1 << 9,
    FunctionIsHiddenInStackTraces = 1 << 10,
    FunctionHasExplicitName = 1 << 11,
    FunctionIsModuleBody = 1 << 12,
    FunctionIsSloppy = 1 << 13,
    FunctionIsDeclaredGenericEntrypoint = 1 << 14,
    FunctionHasNoFieldNamesOnPrototype = 1 << 15,
};

static constexpr uint32_t ParameterIsAnnotated = 1u << 31;
static constexpr uint32_t ParameterIsCalledOnly = 1u << 30;
static constexpr uint32_t untypedParameterIsCalledOnly = none & ~ParameterIsCalledOnly;

struct Function {
    uint32_t name;
    uint32_t module;
    uint32_t position;
    FunctionKind kind;
    uint32_t flags;
    Span captures;
    uint32_t thisType;
    Span parameterTypes;
    Span blocks;
    Span values;
    uint32_t length;
    uint32_t resultType;
    uint32_t sourceStart;
    uint32_t sourceEnd;
};

#define FOR_EACH_AOT_TYPED_OPCODE(v) \
    v(Parameter) \
    v(Undefined) \
    v(Null) \
    v(Boolean) \
    v(Integer) \
    v(Double) \
    v(String) \
    v(BigInt) \
    v(FunctionReference) \
    v(GetModuleVariable) \
    v(PutModuleVariable) \
    v(GetGlobal) \
    v(GetGlobalForTypeOf) \
    v(PutGlobal) \
    v(NewObjectWithShape) \
    v(GetField) \
    v(PutField) \
    v(NewClosure) \
    v(NewClosureOnce) \
    v(GetById) \
    v(PutById) \
    v(InById) \
    v(DeleteById) \
    v(GetByVal) \
    v(PutByVal) \
    v(InByVal) \
    v(DeleteByVal) \
    v(NewArray) \
    v(NewObject) \
    v(Add) \
    v(Sub) \
    v(Mul) \
    v(Div) \
    v(Mod) \
    v(Pow) \
    v(BitAnd) \
    v(BitOr) \
    v(BitXor) \
    v(LShift) \
    v(RShift) \
    v(URShift) \
    v(Negate) \
    v(ToNumber) \
    v(ToNumeric) \
    v(BitNot) \
    v(Less) \
    v(LessEq) \
    v(Greater) \
    v(GreaterEq) \
    v(StrictEq) \
    v(Eq) \
    v(LogicalNot) \
    v(ToBoolean) \
    v(TypeOf) \
    v(InstanceOf) \
    v(ToString) \
    v(ToPropertyKey) \
    v(IsUndefinedOrNull) \
    v(IsInstanceOfClass) \
    v(Strcat) \
    v(IteratorOpen) \
    v(IteratorHasNext) \
    v(IteratorValue) \
    v(IteratorClose) \
    v(Call) \
    v(Construct) \
    v(CallMethod) \
    v(CallDirect) \
    v(Cast) \
    v(CatchValue) \
    v(LoopHint) \
    v(Operation) \
    v(Await) \
    v(GetExternalModule) \
    v(Yield) \
    v(GetResumeMode) \
    v(ConstantSymbol)

enum class Opcode : uint32_t {
#define AOT_TYPED_DECLARE_OPCODE(name) name,
    FOR_EACH_AOT_TYPED_OPCODE(AOT_TYPED_DECLARE_OPCODE)
#undef AOT_TYPED_DECLARE_OPCODE
};

static constexpr uint32_t opcodeNamesHash = [] {
    uint32_t hash = 2166136261u;
    constexpr const char* names =
#define AOT_TYPED_OPCODE_NAME_AND_LINE_FEED(name) #name "\n"
        FOR_EACH_AOT_TYPED_OPCODE(AOT_TYPED_OPCODE_NAME_AND_LINE_FEED)
#undef AOT_TYPED_OPCODE_NAME_AND_LINE_FEED
        ;
    for (const char* character = names; *character; ++character)
        hash = (hash ^ static_cast<uint8_t>(*character)) * 16777619u;
    return hash;
}();

enum class CastReason : uint32_t {
    Store,
    As,
    NonNull,
    Claim,
    Flow,
    Upcast,
    Coerce,
    CoerceFreshObject,
    CoerceFreshGraph,
    AsFreshObject,
    Member,
    AsIfConvertible,
    AsFreshObjectIfConvertible,
};
enum class Freshness : uint32_t { None, TopLevel, WholeGraph };
static constexpr unsigned freshnessShift = 8;
static constexpr uint32_t CastReportsMismatchAtCaller = 1 << 10;
static constexpr unsigned mismatchedArgumentIndexShift = 11;
static constexpr uint32_t CastKeepsUnconvertibleObjects = 1 << 20;

static constexpr uint32_t PredictionIsClosure = 1u << 31;
static constexpr uint32_t PredictionIsByKey = 1u << 30;
static constexpr uint32_t ReceiverHasSingleStructure = 1u << 29;
static constexpr uint32_t VariableIsKnownInitialized = 1;
static constexpr uint32_t CallIsInTailPosition = 1;
static constexpr uint32_t CallIsFusedWithMap = 0;

struct Value {
    Opcode opcode;
    uint32_t position;
    uint32_t children[3];
    uint32_t immediates[2];
    Span arguments;
    uint32_t type;
    uint32_t prediction;
};

struct Target {
    uint32_t block;
    Span arguments;
};

enum class TerminatorKind : uint32_t {
    Jump,
    Branch,
    Switch,
    Return,
    Throw,
    Invoke,
    Unreachable,
};

struct Block {
    Span parameters;
    Span values;
    TerminatorKind terminator;
    uint32_t value;
    Span targets;
    Span cases;
};

static constexpr uint32_t programLogsTypeErrors = 1;

struct Header {
    uint32_t magic;
    uint32_t version;
    uint32_t sizeInBytes;
    uint32_t opcodeNamesHash;
    uint32_t builtinsHash;
    uint32_t flags;
    Section bytes;
    Section words;
    Section strings;
    Section declaredTypes;
    Section objectParts;
    Section shapes;
    Section fields;
    Section methods;
    Section variables;
    Section modules;
    Section functions;
    Section blocks;
    Section values;
    Section targets;
    Span extendedGlobals;
    Span prototypePropertyNames;
    Span builtinModules;
    Span builtinFunctions;
    Span functionsByKey;
    Span entries;
    Span regExpLiterals;
    Span sites;
    Span webAssemblyModules;
    uint32_t recordSizes[12];
};

static constexpr uint32_t recordSizes[12] = {
    sizeof(String), sizeof(DeclaredType), sizeof(ObjectPart), sizeof(Shape), sizeof(Field), sizeof(Method),
    sizeof(Variable), sizeof(Module), sizeof(Function), sizeof(Block), sizeof(Value), sizeof(Target),
};

} } } } // namespace JSC::AOT::Typed::Format

#endif // ENABLE(AOT)
