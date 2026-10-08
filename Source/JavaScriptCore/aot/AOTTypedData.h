/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <span>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

// What a call may change of the memory that compiled code reads, from the least to the most. No level says anything of shape words:
// whether those are written is a question of its own, see Function::writesShapeWords.
enum class Writes : uint8_t {
    // Nor does it read any, trap or throw: it may be run earlier, later, more or less often.
    Pure,
    Nothing,
    // What only the engine looks into, and whether a string is a rope. What makes an object is of this kind: it makes another
    // one each time, so it is not for the back end to run it more or less often.
    RuntimeState,
    // The bytes of buffers, which are the elements of typed arrays. Every view is as long as it was and has its bytes where they were.
    TypedArrayProperties,
    // The fields of the objects of the program, of environments and of the frames of coroutines, module variables, order words: what
    // compiled code stores to by a type. No code that is not known has run, and the other objects of the engine (elements and lengths
    // of arrays, lengths of typed arrays, maps) are as they were.
    Fields,
    // Every field, element and variable.
    Heap,
};

enum class Store : uint8_t {
    Uint8,
    Int8,
    Uint16,
    Int16,
    Int32,
    Uint32,
    Int64,
    Float,
    Double,
    Cell,
    JSValue,
    OptionalInt64,
    OptionalDouble,
    // A byte, zero or one.
    Boolean,
};

enum class LayoutKind : uint8_t {
    Record,
    Instance,
    Environment,
    Closure,
};

// What the language sees as an object. It has a shape word.
inline bool isObjectLayout(LayoutKind kind) { return kind == LayoutKind::Record || kind == LayoutKind::Instance; }

// An optional number is kept as a number. These stand for what is not one: no integer that is kept has these values, and no
// double of the language has these bits.
static constexpr int64_t undefinedInteger = 0x4000ll << 48;
static constexpr int64_t nullInteger = 0x4001ll << 48;
static constexpr int64_t absentInteger = 0x4002ll << 48;
static constexpr uint64_t undefinedDouble = 0x7ffaull << 48;
static constexpr uint64_t nullDouble = 0x7ffbull << 48;
static constexpr uint64_t absentDouble = 0x7ffcull << 48;

// An object of a Record or Instance layout:
//      0  JSCell
//      8  the order word, 32 bits: which of the orders of its layout its keys are in, how many properties of that order exist, flags
//     12  the shape word, 32 bits: the number of its layout among those that are allocated, negated
//     16  the fields
// Where the two words are, an ordinary object has its butterfly. Shapes are numbered from 1. So `word + last < count`, unsigned, in 32
// bits, holds exactly of the shapes first..last: the upper half of a pointer is a small number or zero, for which it does not hold
// (count <= last), so no other object passes for one of a shape.
//
// Compiled code reads the shape word only where a value is held to a type (Cast, and what tests its result), where an optional field is
// not in every member of a view, and to tell which method to call. An access of a field tests nothing: the type of the receiver is a
// fact. So an object stays as its layout says for as long as it lives. What would change that is refused, see AOTTypedObject.h.
static constexpr uint32_t shapeWordOf(uint32_t shape) { return 0 - shape; }
static constexpr uint32_t shapeOf(uint32_t shapeWord) { return 0 - shapeWord; }
static constexpr uint32_t offsetOfOrderWord = 8;
static constexpr uint32_t offsetOfShapeWord = 12;
static constexpr uint32_t offsetOfFirstField = 16;
// The order in the low 16 bits, how far it has got in the next 12. Only what lists the keys looks at these.
static constexpr uint32_t maxFieldsInOrder = (1 << 12) - 1;
static constexpr uint32_t orderWordOf(uint32_t order, uint32_t count) { return order | count << 16; }
static constexpr uint32_t orderOf(uint32_t orderWord) { return orderWord & 0xffff; }
static constexpr uint32_t countOf(uint32_t orderWord) { return orderWord >> 16 & maxFieldsInOrder; }
static constexpr uint32_t oneMoreProperty = 1 << 16;
// It has properties its layout has no field for. They are kept aside, by the object.
static constexpr uint32_t orderWordHasExtraProperties = 1u << 28;
// Object.freeze(), seal() and preventExtensions(). Only objects of layouts the program is seen to hand to one of them can be: stores to
// those test the first bit, and stores that create a property the last. What is frozen is sealed, and what is sealed cannot be extended.
static constexpr uint32_t orderWordIsFrozen = 1u << 29;
static constexpr uint32_t orderWordIsSealed = 1u << 30;
static constexpr uint32_t orderWordIsNotExtensible = 1u << 31;

// `#name` of a class that is laid out, spelled `\xFE#name@class` by the front end. No text has that byte, so no key of the program is such a name.
inline bool isPrivateName(std::span<const uint8_t> name) { return !name.empty() && name[0] == 0xfe; }

static constexpr uint32_t fieldIsOptional = 1;
static constexpr uint32_t fieldIsImmutable = 4;
static constexpr uint32_t fieldAllowsUndefined = 8;
static constexpr uint32_t fieldAllowsNull = 16;
// Its type is written down: what code that was not compiled with the type stores to it is coerced as by `as`.
static constexpr uint32_t fieldIsAnnotated = 32;

// A type at run time: for the store to a field from code that was not compiled with the type, and for a cast.
struct RuntimeShapeTest {
    // With i = first + count - 1 - shape, unsigned: i < count, and that is all if `set` is UINT32_MAX.
    uint32_t first;
    uint32_t count;
    // Otherwise bit `set + i` of the words at Data::shapeSetsOffset has to be set too. See ShapeTest in AOTTypedIR.h.
    uint32_t set;
    // The shape of the layout of the type itself, which a plain object that `as` is applied to is made one of. Zero: nothing is made as
    // the type itself, and the object is made one of a member that is a record.
    uint32_t shapeOfView;
};

struct RuntimeType {
    uint16_t primitives; // Primitive::*
    bool allowsAnyObject;
    bool allowsAnyFunction;
    // One of the views does: any object is of the type. The views say which objects of the program are accessed in place, and what `as`
    // makes of a plain object. See Layout::admitsForeignObjects in AOTTypedIR.h.
    bool admitsForeignObjects;
    int64_t minInteger;
    int64_t maxInteger;
    uint32_t numberOfViews;
    uint32_t numberOfJSTypes;
    // Of an array: the offset in Data of the RuntimeType its elements are expected to have. Zero: none.
    uint32_t elements;
    const RuntimeShapeTest* views() const { return reinterpret_cast<const RuntimeShapeTest*>(this + 1); }
    const uint32_t* jsTypes() const { return reinterpret_cast<const uint32_t*>(views() + numberOfViews); }
};

// A string or a number that a literal type stands for.
struct LiteralData {
    uint32_t isNumber;
    uint32_t string;
    double number;
};

struct FieldData {
    uint32_t name;
    uint32_t offset;
    Store store;
    uint32_t flags;
    // The offsets of RuntimeTypes in Data. What the slot holds, which is the widest of the types of the fields that share it: no more than
    // that is ever in it.
    uint32_t type;
    // What the type of this layout says the property is.
    uint32_t ownType;
    // No RuntimeType tells one string from another. Where the type of the property allows only certain strings, or only certain numbers,
    // these are they: with none of a kind listed, `ownType` says all there is to say of that kind. Compiled code relies on none of it.
    // It is what tells the parts of a union apart when a plain object is made one of them.
    uint32_t numberOfLiterals;
    uint32_t literalsOffset;
};

// The fixed fields of the frame of an async function. The arguments and the spill slots follow.
namespace CoroutineFrame {
static constexpr uint32_t stateField = 0;
static constexpr uint32_t promiseField = 1;
static constexpr uint32_t resumeClosureField = 2;
static constexpr uint32_t resumeValueField = 3;
static constexpr uint32_t resumeErrorField = 4;
static constexpr uint32_t firstArgumentField = 5;
// The state of a coroutine that has completed. 0: it has not started. Otherwise the number of the suspension point.
static constexpr int32_t completedState = -1;
}

// The name is computed: MethodData::name numbers the variable of the class that holds it.
static constexpr uint32_t methodHasComputedName = 1;
// Its closure has the environment of the class object.
static constexpr uint32_t methodTakesEnvironment = 2;
// See Format::MethodMayBeReplaced.
static constexpr uint32_t methodMayBeReplaced = 4;

struct MethodData {
    uint32_t name;
    // The generic entrypoint.
    uint32_t function;
    uint32_t kind; // Format::MethodKind
    uint32_t flags;
};

struct LayoutData {
    static constexpr uint32_t noBase = UINT32_MAX;

    LayoutKind kind;
    // Of the layout among all layouts, and among those that are allocated.
    uint32_t index;
    uint32_t shape;
    uint32_t name;
    uint32_t size;
    uint32_t base;
    bool mayBeFrozen;
    // Of a class, see Format::ShapeFlag.
    bool isLaidOut;
    bool isEvaluatedOnce;
    bool hasForeignRoot;
    bool hasFixedMembers;
    bool mayHaveOtherPrototype;
    // The constructor is handed `new.target` before the arguments.
    bool constructorTakesNewTarget;
    // The orders the compiler expects: for each the offset in Data of a 16-bit length followed by that many 16-bit indexes into the fields.
    uint32_t numberOfOrders;
    uint32_t ordersOffset;
    // The shapes `as` may turn an object of this one into where it is.
    uint32_t numberOfSpecializations;
    uint32_t specializationsOffset;
    // Of a class: the generic entrypoint of the constructor, whose closure is the class object. Else noBase.
    uint32_t classConstructor;
    // Of a class that is not evaluated once: LayoutData::index of the environment the class object closes over, where the variables of the
    // class are. noBase: they are among the module variables.
    uint32_t environment;
    uint32_t numberOfFields;
    // What the prototype object has, then what the class object has.
    uint32_t numberOfMethods;
    uint32_t numberOfStaticMethods;
    // The variables of the class that the run time has to do with: the static fields, then the computed names. The offset is from the
    // start of the module variables, or of the environment.
    uint32_t numberOfStaticFields;
    uint32_t numberOfVariables;
    const FieldData* fields() const { return reinterpret_cast<const FieldData*>(this + 1); }
    std::span<const MethodData> methods() const { return { reinterpret_cast<const MethodData*>(fields() + numberOfFields), numberOfMethods }; }
    std::span<const MethodData> staticMethods() const { return { methods().data() + numberOfMethods, numberOfStaticMethods }; }
    const FieldData* variables() const { return reinterpret_cast<const FieldData*>(staticMethods().data() + numberOfStaticMethods); }
    std::span<const FieldData> staticFields() const { return { variables(), numberOfStaticFields }; }
};

struct AllocationData {
    uint32_t shape;
    // orderWordOf(order, born).
    uint32_t orderWord;
    // In bytes: that of the layout, or with room to be specialized.
    uint32_t size;
    // The function, by the number of its general entry, whose one closure has as its `prototype` the prototype of the objects.
    // LayoutData::noBase: Object.prototype, or that of the class.
    uint32_t constructedBy;
};

// The byte of a function at Data::functionKindsOffset: its Format::FunctionKind, and for a generic entrypoint what the function is
// declared as, which decides what its closures are as objects.
static constexpr uint8_t functionKindMask = 7;
static constexpr uint8_t functionIsAsync = 8;
static constexpr uint8_t functionIsGenerator = 16;
// Which of Instance::typedClosureStructureIDs a closure of the function has.
static constexpr unsigned closureStructureOf(uint8_t kindAndWhatItIsDeclaredAs) { return kindAndWhatItIsDeclaredAs / functionIsAsync & 3; }

// What the program needs at run time: in the program data, read in place.
struct Data {
    static constexpr uint32_t expectedMagic = 0x44505942;

    uint32_t magic;
    uint32_t numberOfStrings;
    uint32_t stringsOffset; // Pairs of offset and length.
    // By shape number: the offset of its LayoutData. Entry 0 is unused, so there is one more than there are shapes.
    uint32_t numberOfLayouts;
    uint32_t layoutsOffset;
    // By the number compiled code allocates with.
    uint32_t numberOfAllocations;
    uint32_t allocationsOffset;
    // By TypeID::index(), which is how compiled code names a type: the offset of its RuntimeType.
    uint32_t numberOfTypes;
    uint32_t typesOffset;
    // By LayoutData::index: the offset of the LayoutData of a class (an Instance layout), with its methods: that of its first shape, or
    // one with shape 0 if nothing allocates it, since its prototype is on the chain of its subclasses all the same. 0 for all else.
    uint32_t numberOfLayoutIndices;
    uint32_t classesOffset;
    // Pairs of the generic entrypoint of a constructor and the LayoutData::index of its class, in the order of the former.
    uint32_t numberOfConstructors;
    uint32_t constructorsOffset;
    // By EngineClass::index, in bytes: whether the front end has seen a class of the program extend the class.
    uint32_t extendedClassesOffset;
    // The bits of the tests that are no range, in 32-bit words: bit n is bit n % 32 of word n / 32.
    uint32_t shapeSetsOffset;
    uint32_t numberOfShapeSetWords;
    uint32_t sizeOfStatics;
    uint32_t numberOfRoots;
    uint32_t rootsOffset;
    uint32_t numberOfFunctions;
    uint32_t functionsOffset; // For each, the number of the string that is its name.
    uint32_t functionKindsOffset; // For each a byte: its Format::FunctionKind, with functionIsAsync and functionIsGenerator.
    uint32_t functionLengthsOffset; // For each, its `length` property, in 16 bits.
    // Pairs of the generic entrypoint of a generator function and that of the function that runs its body, in the order of the former.
    uint32_t numberOfGeneratorFunctions;
    uint32_t generatorFunctionsOffset;
    // Pairs of the generic entrypoint of a plain function that declares `this` to be of a layout, and the number of the allocation that
    // [[Construct]] makes the object with, in the order of the former. The prototype is for whoever constructs to say.
    uint32_t numberOfConstructorFunctions;
    uint32_t constructorFunctionsOffset;

    template<typename T> const T* at(uint32_t offset) const { return reinterpret_cast<const T*>(reinterpret_cast<const uint8_t*>(this) + offset); }
    std::span<const uint8_t> string(uint32_t index) const
    {
        const uint32_t* pair = at<uint32_t>(stringsOffset) + 2 * index;
        return { at<uint8_t>(pair[0]), pair[1] };
    }
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
