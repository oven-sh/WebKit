/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTTypedBuiltins.h"
#include "AOTTypedFormat.h"
#include "AOTTypedID.h"
#include "AOTTypedType.h"
#include "TypedArrayType.h"
#include <wtf/BitVector.h>
#include <wtf/HashSet.h>
#include <wtf/PrintStream.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

#define FOR_EACH_AOT_TYPED_INTERNAL_OPCODE(v) \
    v(GetScope) \
    v(GetArgument) \
    v(CreateRest) \
    v(CallBuiltin) \
    v(HasShapeOf) \
    v(IsClosureOf)

enum class Opcode : uint8_t {
#define AOT_TYPED_DECLARE_OPCODE(name) name,
    FOR_EACH_AOT_TYPED_OPCODE(AOT_TYPED_DECLARE_OPCODE)
    FOR_EACH_AOT_TYPED_INTERNAL_OPCODE(AOT_TYPED_DECLARE_OPCODE)
#undef AOT_TYPED_DECLARE_OPCODE
};

static constexpr unsigned numberOfFormatOpcodes = 0
#define AOT_TYPED_COUNT_OPCODE(name) + 1
    FOR_EACH_AOT_TYPED_OPCODE(AOT_TYPED_COUNT_OPCODE)
#undef AOT_TYPED_COUNT_OPCODE
    ;

ASCIILiteral nameOf(Opcode);

using Format::CastReason;
using Format::FunctionKind;
using Format::MethodKind;
using Format::TerminatorKind;

using Arguments = Vector<ValueID, 4>;

struct Value {
    Opcode opcode { Opcode::Undefined };
    // repOf(type).
    Rep rep { Rep::Void };
    uint32_t position { 0 };
    // What the value is: given by the front end, or by the pass that made the value, which copies a type that is there. Whoever uses the
    // value relies on it. So the operation sees to it: where what it does by itself does not make sure of the type (a property read
    // the general way, a call of a function that is not known, a field whose slot holds more than this view says), it tests what it got
    // and throws a TypeError.
    TypeID type;
    // What the checker expects the value to be, where that is more than the type says: as a rule the value of an expression of
    // JavaScript, whose type is anything. It may be wrong. Nothing relies on it: the verifier does not look at it, no pass derives one
    // from another, and no summary goes by it. Only the lowering of an operation that is carried out the general way reads those of
    // its own operands: it tests for what is expected, does that in place, and does the general thing otherwise. None: nothing is expected.
    TypeID prediction;
    // Likewise: the function that the value is expected to be a closure of.
    FunctionID predictedClosure;
    std::array<ValueID, 3> children;
    std::array<uint32_t, 2> immediates { Format::none, Format::none };
    Arguments arguments;

    ValueID child(unsigned index) const { return children[index]; }
    // Whether something is expected of this value that is not expected of the other: whoever took the other in its place would lose that.
    // A pass that finds the two to be one value leaves this one as an Upcast of the other then, which is no code.
    bool hasPredictionUnlike(const Value& other) const { return (prediction || predictedClosure) && (prediction != other.prediction || predictedClosure != other.predictedClosure); }
    // For a value that a pass makes to stand for the other, or puts in its place under its number.
    void takePredictionOf(const Value& other)
    {
        prediction = other.prediction;
        predictedClosure = other.predictedClosure;
    }
    bool boolean() const { return immediates[0]; }
    uint64_t bits() const { return static_cast<uint64_t>(immediates[1]) << 32 | immediates[0]; }
    int64_t integer() const { return static_cast<int64_t>(bits()); }
    void setBits(uint64_t bits)
    {
        immediates[0] = static_cast<uint32_t>(bits);
        immediates[1] = static_cast<uint32_t>(bits >> 32);
    }
    StringID string() const { return StringID(immediates[0]); }
    FunctionID function() const { return FunctionID(immediates[0]); }
    VariableID variable() const { return VariableID(immediates[0]); }
    LayoutID layout() const { return LayoutID(immediates[0]); }
    uint32_t field() const { return immediates[1]; }
    // Of NewObjectWithShape: among Function::allocations until these are linked, then among Program::allocations.
    uint32_t allocation() const { return immediates[1]; }
    CastReason castReason() const { return static_cast<CastReason>(immediates[1]); }
    // Of a Cast for the reason Coerce: the type after `as`. See Format::CastReason.
    TypeID coercedType() const { return TypeID(immediates[0]); }
    // Of a Cast that stands for the access of a property of its operand, and so turns away `undefined` and `null`: the name. See resolveCalls().
    StringID accessedName() const { return castReason() == CastReason::Coerce ? StringID() : string(); }
    BlockID parameterBlock() const { return BlockID(immediates[0]); }
    uint32_t parameterIndex() const { return immediates[1]; }
    // Of CallBuiltin: the entry of the table that holds, and the class of the receiver or of what is made (EngineClass::index), if any.
    BuiltinID builtin() const { return BuiltinID(immediates[0]); }
    uint32_t engineClass() const { return immediates[1]; }

    template<typename Functor>
    void forEachChild(const Functor& functor) const
    {
        for (ValueID child : children) {
            if (child)
                functor(child);
        }
        for (ValueID argument : arguments)
            functor(argument);
    }

    template<typename Functor>
    void forEachChild(const Functor& functor)
    {
        for (ValueID& child : children) {
            if (child)
                functor(child);
        }
        for (ValueID& argument : arguments)
            functor(argument);
    }
};

struct Target {
    BlockID block;
    Arguments arguments;
};

struct Terminator {
    TerminatorKind kind { TerminatorKind::Unreachable };
    ValueID value;
    Vector<Target, 2> targets;
    Vector<int64_t> cases;

    template<typename Functor>
    void forEachChild(const Functor& functor)
    {
        if (value)
            functor(value);
        for (Target& target : targets) {
            for (ValueID& argument : target.arguments)
                functor(argument);
        }
    }
};

struct Block {
    Vector<ValueID> parameters;
    Vector<ValueID> values;
    Terminator terminator;
};

static constexpr uint32_t FunctionIsGenericEntrypoint = 1 << 16;
// A general entry that is handed how many arguments there are and where they are, not the arguments.
static constexpr uint32_t FunctionUsesArgumentBuffer = 1 << 17;
// Code outside the program may call it as it is, without a general entry in between.
static constexpr uint32_t FunctionHasUnknownCallers = 1 << 18;

// How objects come to be, at one or several places of the program.
struct Allocation {
    LayoutID layout;
    // They are made by `new` on this function, which is declared where there is one closure of it, and have the `prototype` of that
    // closure for their prototype. None: Object.prototype, or the prototype of the class.
    FunctionID constructedBy;
    // The order in which the properties are expected to be created, as indexes into Layout::fields.
    Vector<uint16_t> order;
    // The first so many of them are created by the stores that follow the allocation. Every other field is absent at first.
    uint16_t born { 0 };

    friend bool operator==(const Allocation&, const Allocation&) = default;
};

struct Function {
    StringID name;
    ModuleID module;
    uint32_t position { 0 };
    FunctionKind kind { FunctionKind::Plain };
    uint32_t flags { 0 };
    // Its `length` property.
    uint32_t length { 0 };
    LayoutID environment;
    // Of a constructor, and of its general entry: the class.
    LayoutID constructedClass;
    // functionIsAsync, functionIsGenerator: what it is declared as, and so what its closures are as objects. Also of its general entry.
    // It stays when the splitting of coroutines takes the flags away.
    uint8_t declaredAs { 0 };
    // From the splitting of coroutines on, of what was split: the function that runs its body.
    FunctionID resumeFunction;
    // From linkAllocations() on, of a plain function that is a value and declares `this` to be of a layout that objects are made of: how
    // [[Construct]] makes the object where whoever constructs does not know the function. None: an ordinary object.
    uint32_t allocationOfThis { Format::none };
    // None: anything.
    TypeID thisType;
    TypeID resultType;
    Vector<TypeID> parameterTypes;
    // By parameter: its type is written down. What is passed and does not fit is coerced as by `as`. Otherwise it is turned away.
    BitVector annotatedParameters;
    // See Value::allocation().
    Vector<Allocation> allocations;
    // What a call of it may change, all it calls included.
    Writes writes { Writes::Heap };
    // Whether a call of it may turn an object into one of a specialization of its layout: it comes to an `as` or `!` that does, or to
    // code that is not known, which may. A specialization is a member of every view its base layout is a member of, so a shape test
    // that has passed stays passed. Only that an object is not of a shape has to be found out again after such a call.
    bool writesShapeWords { true };
    // The same two of a call that comes back. What the function does on its way to throwing is nothing to a call that no handler
    // watches: control does not come back there. Never more than the two above.
    Writes writesIfItReturns { Writes::Heap };
    bool writesShapeWordsIfItReturns { true };
    Vector<Block> blocks;
    Vector<Value> values;

    bool has(uint32_t flag) const { return flags & flag; }
    bool isGenericEntrypoint() const { return has(FunctionIsGenericEntrypoint); }
    // Written in TypeScript. In any other function every value that is not known by its operation is anything.
    bool isTyped() const { return has(Format::FunctionIsTyped); }
    // Why what is passed for the parameter is held to its type.
    CastReason reasonToHoldParameter(unsigned index) const { return annotatedParameters.get(index) ? CastReason::As : CastReason::Store; }
    uint32_t allocationOf(Allocation&&);
    Value& at(ValueID id) { return values[id.index()]; }
    const Value& at(ValueID id) const { return values[id.index()]; }
    Block& at(BlockID id) { return blocks[id.index()]; }
    const Block& at(BlockID id) const { return blocks[id.index()]; }
};

// Declared with the syntax of a method: an object of a class has it on its prototype.
static constexpr uint32_t fieldIsMethod = 64;
// Of a variable in an environment: as Variable::isNeverSeenUninitialized and Variable::isStoredOnce.
static constexpr uint32_t fieldIsNeverSeenUninitialized = 128;
static constexpr uint32_t fieldIsStoredOnce = 256;

// Which shapes pass. Program::passes() says it in code.
//
// A range, `set == UINT32_MAX`: the `count` shapes first .. last(). No count: none.
//
// A set, any other `set`: of the shapes first .. last() ONLY those that have a bit. Here `count` is how far the set reaches, not how many
// pass. WHOEVER GOES BY `first` AND `count` WITHOUT LOOKING AT `set` LETS OBJECTS PASS THAT ARE NOT OF THE TYPE, and what follows reads
// their bytes as fields. All sets share one row of bits, Program::shapeSetBits; at run time these are 32-bit words from
// Instance::typedShapeSets on (Data::shapeSetsOffset). `set` is the number of the first bit of this set in the row, a multiple of 32. The
// bits go from the last shape down: shape `last() - i` has bit `set + i`, for i < count. Bits 0 and count - 1 are always set.
//
// Compiled, with `word` the 32-bit shape word of a cell (0 - shape; in a cell that is no object of the program the upper half of a pointer,
// or zero, see AOTTypedData.h):
//     i = word + last()                                           in 32 bits, wrapping
//     if (i >= count) fail                                        unsigned; a range is done here, and with count 1 it is `i != 0`
//     bits = load32(typedShapeSets + set / 8 + (i >> 5) * 4)      `set / 8` is a constant
//     if (!(bits >> (i & 31) & 1)) fail
// Only the comparison keeps the load within the row: the load depends on it (not IsSafe), or `i` is clamped first. With count <= 64 the
// bits may be an immediate instead of a load. Tests of several views: merge ranges that are adjacent, placement tries to make them so.
struct ShapeTest {
    uint32_t first { 0 };
    uint32_t count { 0 };
    uint32_t set { UINT32_MAX };

    bool isRange() const { return set == UINT32_MAX; }
    bool passesNothing() const { return !count; }
    uint32_t last() const { return first + count - 1; }
};

// A string or a number that a literal type stands for.
struct Literal {
    // None: it is a number.
    StringID string;
    double number { 0 };
};

struct Field {
    StringID name;
    // What the type of this layout says the property is. None: anything.
    TypeID type;
    uint32_t flags { 0 };
    // See Format::Field::literals. Only the run time looks at them.
    Vector<Literal> literals;
    // From placement on. The fields of that name of a view and of its members are one slot: one offset, one way to store. It holds
    // what the widest of their types says, `stored`. Where that is more than `type`, a read tests.
    SlotID slot;
    TypeID stored;
    Store store { Store::JSValue };
    uint32_t offset { 0 };
    // Not every member of this layout has a field for the property. It may be optional. It may be required and be on the prototype of
    // a class (a method, an accessor) or on Object.prototype (`toString`). An access tests the shape for the members that have the
    // field. Of any other object it is read the general way (extra properties, prototype chain), and what that yields is held to the
    // type of the value. A store to any other object goes to the run time.
    bool isLackedBySomeMember { false };
    ShapeTest membersWithIt;
};

struct Method {
    // None: the name is computed.
    StringID name;
    FunctionID function;
    MethodKind kind { MethodKind::Method };
    // The variable of the class that holds a computed name, see Layout::environment.
    uint32_t key { Format::none };
    // See Format::MethodMayBeReplaced.
    bool mayBeReplaced { false };
};

struct StaticField {
    StringID name;
    // A variable of the class, see Layout::environment.
    uint32_t variable { Format::none };
    // Of a module variable. Of a field of an environment, that field says so.
    bool isAnnotated { false };
};

// Of the objects of a type of the program: an interface, a type alias, the type of a literal, a class. Also of environments, closures
// and the frames of coroutines, which the language does not see as objects.
//
// It is two things. Objects are allocated with it, if anything does that. And it is a view: the way to look at the objects in places of
// the type, which were allocated with one of its members.
//
// Every class has one, of the kind Instance, which also says what the class is made of. See Format::Shape.
struct Layout {
    LayoutKind kind { LayoutKind::Record };
    StringID name;
    LayoutID base;
    Vector<Field> fields;
    // Of a class: its constructor, whose closure is the class object, what the prototype object has, what the class object has.
    // `methods` of a Record: the accessors that an object literal of the type declares. There is no field of the name: the object gets the
    // accessor as an extra property where the literal is evaluated, and may come to have something else under the name. So all this
    // says is that an access by the name may run code: see onlyFieldsSupply(). The run time is not told.
    FunctionID constructor;
    Vector<Method> methods;
    Vector<Method> staticMethods;
    Vector<StaticField> staticFields;
    // Of a class that is not evaluated once: of the environment its class object closes over. A variable of the class is a field of that.
    // None: a variable of the class is a module variable.
    LayoutID environment;
    // See Format::ShapeFlag.
    bool isLaidOut { false };
    bool isEvaluatedOnce { false };
    bool hasForeignRoot { false };
    bool hasFixedMembers { false };
    bool mayHaveOtherPrototype { false };
    // The prototype object has a method whose name is computed and may be any string: what an object of the class, or of one that extends
    // it, finds under a name is not known here.
    bool hasMethodOfUnknownName { false };
    // The same of the class object. See Format::ShapeHasStaticMemberOfUnknownName.
    bool hasStaticMemberOfUnknownName { false };
    // From before types are frozen. A cast may turn an object of the class, or of one that extends it, into one of the layout of a record,
    // where it is. It keeps its prototype, and its shape no longer says what made it.
    bool instancesMayBecomeRecords { false };
    bool isAllocated { false };
    // The program is seen to freeze or seal objects that are looked at as a type this is a member of. A store to a field tests for that.
    // Freezing an object of any other layout is refused.
    bool mayBeFrozen { false };
    // As a view. The checker has seen an object that the program has not laid out, which honestly is of the type, get into a place of the
    // type: an object of the host, of a class of a `.d.ts` file or of JavaScript. It can be neither copied nor turned away. So all that a
    // type with this view says of an object is that it is one: ANY object is of the type, also one of the program that is no member.
    // An access applies `test`: what passes is accessed in place, as through any view. Of anything else the object itself is asked, the
    // general way, which may run code, and what that yields is held to its type. Nothing may conclude from the type that an object has the
    // fields, or that it is of the program.
    bool admitsForeignObjects { false };
    // An object of it, and not `undefined` or `null`: Type::view(this layout). Of every layout, from before types are frozen.
    TypeID type;
    // The frame of a coroutine: the run time stores to its fixed fields, see CoroutineFrame.
    bool isCoroutineFrame { false };
    // The allocated layouts whose objects get into places of this type untested, itself among them if it is allocated. The front end
    // says which, from what the checker has found assignable. Not each has a field for every property: see Field::isLackedBySomeMember. Sorted.
    Vector<LayoutID> members;
    // What `as` may turn an object into, where it is, that was allocated with this layout: layouts that have all its slots. Sorted.
    Vector<LayoutID> specializations;

    // From placement on.
    uint32_t size { 0 };
    // What an allocation takes that may be specialized: the size of the largest.
    uint32_t sizeWithRoomToSpecialize { 0 };
    // Its number among the allocated layouts, from 1. Zero: no objects.
    uint32_t shape { 0 };
    // Whether an object is of a member.
    ShapeTest test;
    // Of a class: whether an object was made by it or by a class that extends it. Not `test`: a record may be assignable to a class.
    ShapeTest instances;
    // As a view: whether an object is of a layout that is no member, and a cast to the view turns objects of that into members where they are.
    ShapeTest retagSources;
    // The keys of an object are in the order its properties were created in, which is no part of where the fields are, nor of the
    // shape. An order is a list of indexes into `fields`. An object says which it goes by and how far it has got: the order word, see
    // AOTTypedData.h. These are the orders the allocations of the program expect. The run time adds others.
    Vector<Vector<uint16_t>> orders;

    std::optional<uint32_t> fieldNamed(StringID) const;
};

struct Slot {
    TypeID type;
    Store store { Store::JSValue };
    uint32_t offset { 0 };
};

struct Variable {
    StringID name;
    ModuleID module;
    // None: anything.
    TypeID type;
    // It holds `undefined` from before any code runs. Otherwise nothing, until it is initialized: a read in between throws.
    bool startsUndefined { false };
    // There is no read in between: see Format::Initialization::NeverSeen. So a read tests nothing and cannot throw, and nothing has to
    // stand for "nothing" in the variable, which is kept as its type alone says.
    bool isNeverSeenUninitialized { false };
    // And it has one store. To every function but the one with the store it never changes.
    bool isStoredOnce { false };
    bool mayBeSeenUninitialized() const { return !startsUndefined && !isNeverSeenUninitialized; }
    // The program has exactly one store to it, of a closure of this function that captures nothing, and no class object shows it as a
    // static field, which code that is not known may store to.
    FunctionID onlyFunction;
    Store store { Store::JSValue };
    uint32_t offset { 0 };
};

struct Module {
    StringID path;
    FunctionID initializer;
};

class Program final {
    WTF_MAKE_TZONE_ALLOCATED(Program);
    WTF_MAKE_NONCOPYABLE(Program);
public:
    Program() = default;

    static std::unique_ptr<Program> tryRead(std::span<const uint8_t>, ASCIILiteral& whyFailed);

    std::span<const uint8_t> string(StringID id) const { return strings[id.index()]; }
    // What the table knows the string as, if it does.
    BuiltinName builtinName(StringID id) const { return m_builtinNames[id.index()]; }
    // The other way around. Nothing: the program has no such string.
    StringID stringOf(BuiltinName name) const { return name.index() < m_stringsOfBuiltinNames.size() ? m_stringsOfBuiltinNames[name.index()] : StringID(); }
    Function& at(FunctionID id) { return functions[id.index()]; }
    const Function& at(FunctionID id) const { return functions[id.index()]; }
    Layout& at(LayoutID id) { return layouts[id.index()]; }
    const Layout& at(LayoutID id) const { return layouts[id.index()]; }
    Slot& at(SlotID id) { return slots[id.index()]; }
    const Slot& at(SlotID id) const { return slots[id.index()]; }
    const Type& at(TypeID id) const { return types.at(id); }
    const Type& typeOf(const Function& function, ValueID value) const { return types.at(function.at(value).type); }
    // Whether an object that is looked at as the one layout says can be looked at as the other says: every member of the one is a member
    // of the other, and the other admits foreign objects if the one does. Then it takes any object.
    bool isUpcast(LayoutID, LayoutID) const;
    // Whether a cast for the reason has nothing to do for a value of the one type: it may be left out. `as` and `!` make something of an
    // object that is of a type only because the type admits foreign objects. What they do goes by the value and by the type they cast
    // to, never by how much a pass has found out about the operand.
    bool isHeldAlready(const Type& from, const Type& to, CastReason reason) const { return from.isWithin(to, *this, reason != CastReason::As); }
    bool passes(const ShapeTest& test, uint32_t shape) const
    {
        uint32_t i = test.last() - shape;
        return i < test.count && (test.isRange() || shapeSetBits.get(test.set + i));
    }
    // Of the global objects for which that makes a difference (see Builtins::mayBeExtended): the classes that a class of the program
    // extends, the objects whose properties the program may replace. The front end says which.
    bool isExtended(BuiltinName global) const { return extendedGlobals.get(global.index()); }
    BitVector extendedGlobals;

    StringID intern(std::span<const uint8_t>);
    // From here on the string `from`, if the program has it, reads `to`. Before any pass has run.
    void replaceString(std::span<const uint8_t> from, std::span<const uint8_t> to);
    std::optional<StringID> find(std::span<const uint8_t>) const;

    void dump(PrintStream&) const;
    void dump(PrintStream&, FunctionID) const;
    void dump(PrintStream&, const Type&) const;

    Vector<uint8_t> buffer;
    Vector<std::span<const uint8_t>> strings;
    Vector<Layout> layouts;
    Vector<Variable> variables;
    Vector<Module> modules;
    Vector<Function> functions;
    FunctionID entry;

    // By string: its number among the identifiers of the whole image, which the caches go by. Zero: it has none.
    Vector<uint32_t> identifierNumbers;
    // The names by which values of unknown layout are accessed.
    Vector<StringID> genericAccessIdentifiers() const;

    TypeTable types;
    LayoutID closureLayout;
    // From the linking of allocations on: every way objects come to be, in the order of the functions.
    Vector<Allocation> allocations;
    // From the effect summaries on. Some layout has specializations, so that an `as` or `!` may write the shape word of an object.
    bool castsMayRetag { false };
    // By layout: turning a plain object into one of it (`as`, `!`) may come to an array and replace elements of that where they are.
    BitVector layoutsCoercedThroughArrays;
    // From before the functions are optimized on. By layout, as a view: a cast to it may turn an object into one of a specialization of
    // its layout where it is. That is the layout itself if objects are made of it, else one of its members.
    BitVector viewsRetaggedTo;
    // From placement on.
    Vector<Slot> slots;
    uint32_t numberOfShapes { 0 };
    // The bits of all tests that are no range, one set after the other. See ShapeTest.
    BitVector shapeSetBits;
    StringID emptyString;
    uint32_t sizeOfStatics { 0 };
    UncheckedKeyHashMap<uint32_t, FunctionID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> genericEntrypoints;

private:
    friend class Preparation;
    friend class Reader;

    Vector<Vector<uint8_t>> m_ownStrings;
    UncheckedKeyHashMap<String, uint32_t> m_stringIDs;
    void noteString(StringID);

    Vector<BuiltinName> m_builtinNames;
    Vector<StringID> m_stringsOfBuiltinNames;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
