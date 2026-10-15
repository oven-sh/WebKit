/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed { namespace Format {

// The program as the front end hands it over: one buffer of 32-bit words, read in place. src/aot/format.rs in Bun mirrors this file.

static constexpr uint32_t magic = 0x52495442;
static constexpr uint32_t version = 35;
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
};

enum DeclaredTypeFlag : uint32_t {
    DeclaredAnyObject = 1 << 0,
    DeclaredAnyFunction = 1 << 1,
    DeclaredIntegers = 1 << 2,
};

struct DeclaredType {
    uint32_t primitives;
    uint32_t flags;
    uint32_t leastIntegerLow;
    uint32_t leastIntegerHigh;
    uint32_t mostIntegerLow;
    uint32_t mostIntegerHigh;
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
    // Of a Record or an Instance, by the name like FieldIsNeverStored. The numbers stored under the name are expected to be int32. It
    // may be wrong, and nothing depends on it but speed and size: see Typed::Store::Int32OrElsewhere.
    FieldIsExpectedInt32 = 1 << 1,
    FieldIsImmutable = 1 << 2,
    // Declared with the syntax of a method.
    FieldIsMethod = 1 << 3,
    // Its type is written down, so what does not fit it is coerced as by `as`.
    FieldIsAnnotated = 1 << 4,
    // Of a variable in an environment. See Initialization::NeverSeen and NeverSeenAndStoredOnce: the second is never set without the first.
    FieldIsNeverSeenUninitialized = 1 << 5,
    FieldIsStoredOnce = 1 << 6,
    // Of a Record or an Instance. No code of the program that is reached stores a property of the name by name, and no class has a member
    // of the name. It goes by the name: every field of the name has the flag, or none has. Such a property does not exist: the field takes
    // no room in any object. No store names it. A read by the name finds what a read of a name without a field finds.
    FieldIsNeverStored = 1 << 7,
    // Of a Record or an Instance. Predictions: they may be wrong, and nothing depends on them but where the field is kept. An object is
    // followed from where it is made (through its constructor, the block that holds it in a local, out of a factory into its callers)
    // to where it is lost sight of. The field is stored to on the way: in every such case and on every path, or in some.
    FieldIsSetAtCreation = 1 << 8,
    FieldMayBeSetAtCreation = 1 << 9,
    // Of an Instance: no constructor defines the field. It stands for an optional property of an interface the class implements and does
    // not declare. A class that extends the class has the flag too, unless it declares the field itself.
    FieldIsAbsentAtBirth = 1 << 10,
    // Of a Record or an Instance that objects are made of: a required field. A fact. From the moment an object has the layout to the first
    // store to the field nobody reads the field, asks whether it is there, or gets hold of the object. It is never deleted. By layout:
    // another layout may have a field of the name without the flag. A layout and a specialization of it have it both or neither.
    FieldIsNeverSeenAbsent = 1 << 11,
    // The type the program declares for the property, aliases seen through, has no `undefined` in it, and is not `any`, `unknown` or `void`.
    // Field::declaredType does not say: it has `undefined` and `null` for every string and object. An IR without the flag claims nothing.
    FieldDoesNotAdmitUndefined = 1 << 12,
};

// Of Field::onlyFunction and Variable::onlyFunction.
enum OnlyFunctionFlag : uint32_t {
    // Whenever the place is read it holds a closure of the function. With neither this nor OnlyFunctionOrNil the function is a prediction:
    // nothing may rely on it.
    OnlyFunctionIsFact = 1 << 0,
    // Of a variable (of a module, in an environment): all its stores are seen, and all it can hold besides is `undefined` or `null`.
    OnlyFunctionOrNil = 1 << 2,
};

enum class LiteralKind : uint32_t {
    String,
    Number,
};

// Three of the words.
struct Literal {
    LiteralKind kind;
    // The number of the string and nothing, or the halves of the double.
    uint32_t low;
    uint32_t high;
};

struct Field {
    uint32_t name;
    uint32_t declaredType;
    uint32_t flags;
    // Among the words: Literals. A declared type does not tell one string from another. If the only strings the type of the property
    // allows are those of literal types, they are listed, and likewise the numbers. With none of a kind listed, the declared type says all
    // there is to say of that kind. No fact: it tells the parts of a union apart when a plain object is made one of them.
    Span literals;
    // The declared type as the checker prints it (`readonly Foo[]`), for error messages. A string, or none.
    uint32_t typeText;
    // The one function of the program whose closures every store the checker sees puts there, or none. Of a Record or an Instance always
    // a prediction.
    uint32_t onlyFunction;
    uint32_t onlyFunctionFlags;
};

enum class MethodKind : uint32_t {
    Method,
    Getter,
    Setter,
};

enum MethodFlag : uint32_t {
    // Of the class object, not of the prototype.
    MethodIsStatic = 1 << 0,
    // Its name is computed, and is a symbol: that is the type of the variable that holds it. Any other computed name may be any string, and
    // then nothing is known of what an object of the class finds under a name.
    MethodKeyIsSymbol = 1 << 1,
    // The program is seen to put something else in its place: to store to the property, define it or delete it, on an object that finds this
    // method under the name, or on the prototype object (of a static one: on the class object). On every method and accessor of that
    // name in the class. Nothing is known of what is found under the name where the search would end here. Without the flag all of that
    // is refused when the program runs.
    MethodMayBeReplaced = 1 << 2,
};

struct Method {
    // None: the name is computed.
    uint32_t name;
    uint32_t function;
    MethodKind kind;
    // MethodFlag.
    uint32_t flags;
    // The variable of the class (see Shape::environment) that holds the computed name, as a property key, from the declaration on.
    uint32_t key;
};

enum ShapeFlag : uint32_t {
    ShapeIsAllocated = 1 << 0,
    ShapeMayBeFrozen = 1 << 1,
    // The rest are of classes.
    // The objects of the class have this layout. Otherwise they are ordinary objects, or what a class makes that is not of the program:
    // the layout has no fields, nothing is allocated with it, and no type names it.
    ShapeIsLaidOut = 1 << 2,
    // The declaration is evaluated at most once, so there is one class object.
    ShapeIsEvaluatedOnce = 1 << 3,
    // It is not laid out and extends something, which makes the object: a class that is not of the program, one that is not laid out,
    // `null`. What that is, is evaluated at the declaration and handed to MakeClass.
    ShapeHasForeignRoot = 1 << 4,
    // Written in TypeScript. The static methods and accessors it declares cannot be replaced, redefined or deleted, the class object keeps
    // its prototype, and gets no property of its own by the name of a static method or accessor it inherits.
    ShapeHasFixedMembers = 1 << 5,
    // It is laid out, and a class that is not extends it. What that class constructs has this layout and the prototype object of that
    // class, which may have anything by the name of a method. Without the flag, constructing it for such a class throws.
    ShapeMayHaveOtherPrototype = 1 << 6,
    // Of any shape, as a way to look at objects: places of the type also hold objects that the program has not laid out, which get there
    // untested. See Layout::admitsForeignObjects.
    ShapeAdmitsForeignObjects = 1 << 7,
    // Of a class: it declares a static field or method whose name is computed and may be any string. What is found on the class object
    // under a name, or on that of a class that extends it, is not known.
    ShapeHasStaticMemberOfUnknownName = 1 << 8,
    ShapeConstructorMayBeReplaced = 1 << 9,
    ShapeMayBeReused = 1 << 10,
    ShapeIsOpenedForLog = 1 << 11,
    ShapeAdmitsOnlyOtherLayouts = 1 << 12,
};

// Every class of the program has a shape of the kind Instance. The class object is the closure of its constructor, and is made by the run
// time from what is here, with the prototype object and the closures of the methods.
//
// A class that is evaluated once and has no foreign root: when the closure is first asked for (FunctionReference) or an object is first
// allocated. Any other class: by the operation MakeClass, at the declaration.
struct Shape {
    ShapeKind kind;
    uint32_t name;
    // Of a class that is laid out: the class it extends, which is laid out too.
    uint32_t base;
    Span fields;
    // Of a class, in the order of the declaration: the methods and accessors, also the static ones and those named `#name`.
    Span methods;
    // Of a class. None: only types name the class, its declaration is no part of the program.
    uint32_t constructor;
    // ShapeFlag.
    uint32_t flags;
    // The allocated layouts whose objects get into places of this type untested. Sorted.
    Span members;
    Span membersByFit;
    // What a cast may turn an object into that was allocated with this layout. Sorted.
    Span specializations;
    // Of a class that is not evaluated once: the layout of the environment of the class, one for each evaluation. The class object captures
    // it and nothing else, and so does the constructor and each of `methods`, or it captures nothing. A variable of the class is a field
    // of it, by its index. What the members need from around the class is in fields of it that are immutable. None: a variable of the
    // class is a module variable.
    uint32_t environment;
    // Pairs of words: name, declared type or none. Only public static fields that have a type annotation.
    Span staticFields;
    // Where the type is declared, for error messages that name two types alike: a string spelled like Module::displayPath, or none, and
    // the line, from 1, or 0. None: an environment, a type that nothing declares, a declaration in a file whose text was not kept.
    uint32_t declaredPath;
    uint32_t declaredLine;
};

// What a module variable holds before anything is stored to it.
enum class Initialization : uint32_t {
    // Nothing: it is in its temporal dead zone.
    Uninitialized,
    // `undefined`, from before any code runs.
    Undefined,
    // Nothing, but nobody gets to see that: but for the function that declares it, no activation of a function that reads it begins
    // before the first store, and in that function no read comes before the store.
    NeverSeen,
    // And the store of its declaration is the only one.
    NeverSeenAndStoredOnce,
    // All bits clear, from before any code runs, which its declared type, that of numbers or of booleans kept raw, reads as 0 or `false`. A read may come first.
    Zero,
};

struct Variable {
    uint32_t name;
    uint32_t module;
    uint32_t declaredType;
    Initialization initialization;
    // As Field::onlyFunction.
    uint32_t onlyFunction;
    uint32_t onlyFunctionFlags;
};

struct Module {
    uint32_t path;
    uint32_t initializer;
    // The path to print in stack traces: relative to the directory the build ran in, with '/' separators.
    uint32_t displayPath;
    // Among the bytes, ULEB128 numbers. Value::position is a UTF-8 byte offset; this turns it into a line and a UTF-16 column.
    //   numberOfLines, then numberOfLines - 1 line lengths in bytes, terminator included
    //   numberOfNonASCIILines, then for each: its line index as a delta from the previous one listed (the first from 0), a count, and
    //   one number per non-ASCII character: (gap << 2) | (length - 2). gap: bytes since the end of the previous non-ASCII character of
    //   the line, or since the line start. length: its UTF-8 length.
    Span lineTable;
    Span printed;
};

enum class FunctionKind : uint32_t {
    Plain,
    Arrow,
    Method,
    // Of a class. It makes no object: for `this` it is handed the one whoever constructs has made, or `undefined` where the class has a
    // foreign root. It yields `undefined`, or the object that takes the place of the one it was handed.
    Constructor,
    Module,
    // Not written by the front end: made by createBoundFunctions(). What `f.bind(t, a)` yields where `f` is known and does not construct: it captures `t` and `a` and calls `f`.
    // As an object it is what a bound function is: no `prototype`, no [[Construct]], "bound " in front of the name.
    Bound,
};

enum FunctionFlag : uint32_t {
    FunctionIsAsync = 1 << 0,
    FunctionIsGenerator = 1 << 1,
    FunctionIsUsedAsValue = 1 << 2,
    FunctionHasRestParameter = 1 << 3,
    FunctionHasThis = 1 << 4,
    // A generator that runs up to its first yield when it is called: the binding of its parameters.
    FunctionStartsEagerly = 1 << 5,
    // Written in TypeScript.
    FunctionIsTyped = 1 << 6,
    // A constructor whose first parameter is `new.target`. Its `length` does not count that. A plain function that asks for `new.target`,
    // by LanguageOperation::GetNewTarget: it has no such parameter.
    FunctionHasNewTarget = 1 << 7,
    // Of the front end's prelude: it implements a built-in. Stack traces hide it, and the inliner takes it whatever its size.
    FunctionIsBuiltin = 1 << 8,
    // Written in one of the host's own modules: see Header::builtinModules.
    FunctionIsOfBuiltinModule = 1 << 9,
    // Its frames only exist because of how the front end has compiled the code: what sees to it that a module is evaluated once.
    FunctionIsHiddenInStackTraces = 1 << 10,
    FunctionNameIsGiven = 1 << 11,
    FunctionIsCodeOfModule = 1 << 12,
    FunctionIsSloppy = 1 << 13,
    FunctionIsGeneralEntry = 1 << 14,
    FunctionKeepsFieldNamesOffItsPrototype = 1 << 15,
};

// In Function::parameterTypes: the type of the parameter is written down, so what does not fit it is coerced as by `as`. The other bits
// number the type.
static constexpr uint32_t ParameterIsAnnotated = 1u << 31;
static constexpr uint32_t ParameterIsOnlyCalled = 1u << 30;
static constexpr uint32_t untypedParameterIsOnlyCalled = none & ~ParameterIsOnlyCalled;

struct Function {
    uint32_t name;
    uint32_t module;
    uint32_t position;
    FunctionKind kind;
    uint32_t flags;
    // Among the words: the types of what the function captures, which it takes before `this`. A capture is an environment (the view of a
    // layout of that kind, exact, never nil) or a copy of a variable that is never stored to again (its declared type).
    Span captures;
    uint32_t thisType;
    Span parameterTypes;
    Span blocks;
    Span values;
    // Its `length` property.
    uint32_t length;
    uint32_t resultType;
    uint32_t placeOfText;
    uint32_t endOfText;
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
    v(GetResumeMode)

enum class Opcode : uint32_t {
#define AOT_TYPED_DECLARE_OPCODE(name) name,
    FOR_EACH_AOT_TYPED_OPCODE(AOT_TYPED_DECLARE_OPCODE)
#undef AOT_TYPED_DECLARE_OPCODE
};

// Of the names of the operations in their order, each followed by a line feed: FNV-1a. A front end that numbers them another way
// is turned away.
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

// A value that does not fit is turned away with a TypeError, but for what is said of As, Flow and Coerce.
enum class CastReason : uint32_t {
    Store,
    // It enters a place whose type is written down. An object that can be made one of the type is: plain data is copied into the layout,
    // an object that was made as something the type extends becomes one of the type where it is.
    As,
    // `x!`: neither `undefined` nor `null`. Nothing is made of anything. Value::immediates[0]: the source text of `x` where that is
    // short and simple (`node.parent`), for the error message. A string, or none.
    NonNull,
    Claim,
    // The second of what As does, and no more.
    Flow,
    // The value is of the type already: nothing is tested, nothing is done.
    Upcast,
    // `x as T`, as it is written. It coerces, as ToObject() does: it makes of the value what it can, and does not turn away what it can
    // make nothing of. People cast first and look afterwards. Value::immediates[0] is T. For every other reason that is the type of the
    // value. Here the type of the value is what comes of the cast whichever way it goes: T, and the objects that x can be.
    //
    // An object, where T has objects: what `As` would make something of is made that, any other stays as it is. Any other value that is
    // not of a kind T has is converted as the language converts: ToNumber, ToString, ToBoolean or ToBigInt where T has one kind of
    // primitive, else ToPrimitive and then the first of ToString, ToNumber, ToBoolean that T has the kind for. There is no
    // conversion to a symbol or to an object: those throw, like a number that T has no room for.
    //
    // Nothing is made of an object that is not of the program: somebody else may hold it, and it stays the object it is. Unless the operand is
    // fresh by the text: see Freshness.
    Coerce,
    // Coerce with a Freshness, as compiled code tells the run time. No Value has them for a reason.
    CoerceFreshObject,
    CoerceFreshGraph,
    AsFreshObject,
};
// Of a Cast for the reason Coerce, in Value::immediates[1] from bit 8 on. Nobody else can hold the object that the operand yields: `{ ...a } as T`,
// `Object.fromEntries(..) as T`: plain data is copied into the layout as `As` does it, and what it holds stays what it is. Nor anything that
// hangs below it: `JSON.parse(text) as T`: that is copied as well.
enum class Freshness : uint32_t { None, TopLevel, WholeGraph };
static constexpr unsigned freshnessShift = 8;
static constexpr uint32_t CastNotesMisfitAtCaller = 1 << 10;
static constexpr unsigned argumentOfMisfitAtCallerShift = 11;

// In Value::prediction: the other bits number a function, not a type.
static constexpr uint32_t PredictionIsClosure = 1u << 31;
// Without that one, on a GetByVal: the other bits number a string. See Header::functionsByKey.
static constexpr uint32_t PredictionIsByKey = 1u << 30;
static constexpr uint32_t ReceiverIsOfOneRow = 1u << 29;
static constexpr uint32_t VariableIsInitialized = 1;
static constexpr uint32_t CallIsInTailPosition = 1;

// NewObjectWithShape: immediates[0] is the layout. children[0] and immediates[1] are both none, or a value and the name of a property:
// of an object that `new` makes for a function it does not know. Then this is a fact. Every function of the program that declares a
// `this` the object is of begins by storing that argument to that property, and nothing else in any file of the program stores to a
// property of that name by name. So the property of this object is never stored by name, or holds the value for good. The type of
// the property is an enum of numbers or lists numbers. How large the object is made goes by it: see Typed::Allocation::discriminant.
struct Value {
    Opcode opcode;
    uint32_t position;
    uint32_t children[3];
    uint32_t immediates[2];
    Span arguments;
    uint32_t type;
    // What the checker expects the value to be, where that is more than its type says: a type, or with PredictionIsClosure a function
    // that it is expected to be a closure of. It may be wrong, and nothing relies on it. None: nothing is expected.
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
    // Typed::builtinsHash of the table the front end was built with.
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
    // Among the words, by name: the classes of the engine that a class of the program extends, going by the checker's type of what stands
    // after `extends`, and `console` if the program does anything with it but read and call its properties.
    Span extendedGlobals;
    // Among the words, strings, sorted, each once: the names the program is seen to define on an object that is written `E.prototype`,
    // whatever E is. `P.N = v`, Object.defineProperty(P, "N", ..), the keys of the literals in Object.defineProperties(P, { .. }) and
    // Object.assign(P, { .. }), and the like with Reflect. Not through an alias, not `P = { .. }`. No fact: it is what the run time
    // lets through. See Typed::PrototypeObject.
    Span prototypePropertyNames;
    // Among the words, pairs, ascending by the first, which is there once: the number of a module in the host's registry, and the function
    // that is the module: plain, without parameters and captures, it yields what the module exports. See Typed::evaluateBuiltinModule().
    Span builtinModules;
    // Among the words, triples, ascending by the first, which is there once: the number the engine or the host has for a function it
    // implements in JavaScript; the function, which captures nothing; its ImplementationVisibility. See Typed::builtinFunction().
    Span builtinFunctions;
    // Among the words, triples, ascending by the first two, which are there once: the name of a property that holds dictionaries; a key; the
    // one function that the program is seen to store under the key in such a dictionary. A prediction, for a GetByVal that is marked with
    // the property, once its key is a constant.
    Span functionsByKey;
    // Among the words: the entry functions, in the order of the entry points, at least one, each once. Each runs in the place of the
    // top-level code of a module. In an Instance one of them runs, once.
    Span entries;
    Span regExpLiterals;
    Span sites;
    uint32_t recordSizes[12];
};

static constexpr uint32_t recordSizes[12] = {
    sizeof(String), sizeof(DeclaredType), sizeof(ObjectPart), sizeof(Shape), sizeof(Field), sizeof(Method),
    sizeof(Variable), sizeof(Module), sizeof(Function), sizeof(Block), sizeof(Value), sizeof(Target),
};

} } } } // namespace JSC::AOT::Typed::Format

#endif // ENABLE(AOT)
