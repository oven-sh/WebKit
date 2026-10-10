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
// What an image that counts tells apart among the plain objects it makes: see State::countPlainAllocation().
enum class PlainAllocationKind : uint8_t { Object, This, Array, Clone };
static constexpr unsigned plainAllocationKindShift = 28;

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
    // Four bytes, of a field of an object that holds numbers which are expected to be int32, and maybe `undefined` or `null`. See
    // elsewhereInt32. It is storage only: the field is of the type it is of, and holds whatever that admits.
    Int32OrElsewhere,
    // Four bytes: the address of a cell, shifted right by four.
    Cell32,
    // Likewise, or 0: absent, 2: null, 10: undefined.
    OptionalCell32,
    // A byte, the low one of the JSValue: 0: absent, 2: null, 6: false, 7: true, 10: undefined.
    OptionalBoolean,
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

// Store::Int32OrElsewhere. From leastInt32InPlace on, the word is the number. Any other number (a fraction, -0, NaN, what is beyond int32,
// these four integers) is among the extra properties of the object, under a private name for the field, and the word says so. An
// entry beside a word that does not say so is stale, and is not looked at.
static constexpr int32_t elsewhereInt32 = INT32_MIN;
static constexpr int32_t absentInt32 = INT32_MIN + 1;
static constexpr int32_t undefinedInt32 = INT32_MIN + 2;
static constexpr int32_t nullInt32 = INT32_MIN + 3;
static constexpr int32_t leastInt32InPlace = INT32_MIN + 4;

// Store::Cell32 and Store::OptionalCell32. Every cell the type of such a slot admits is in a MarkedBlock, so it is 16-byte aligned and within the
// MarkedBlockCage: MarkedBlockCage::encode() and decode(). What is no cell is the low half of its JSValue, which is below every index.
static constexpr unsigned cell32Shift = 4;
static constexpr uint32_t absentCell32 = 0;
static constexpr uint32_t nullCell32 = 2;
static constexpr uint32_t undefinedCell32 = 10;

// Instance::typedStrings: of the strings of the program those that have been asked for as values, found by their number. A table as long as the program costs a page for every
// row that is looked at, and the rows that are looked at are spread over all of it. Open addressing, a word a slot: in the low half the key: the number + 1, 0: free; in the high half the
// cell: a Cell32, with wide references its place in Instance::typedWideStrings. Slot 0 is no string's: its key is that of none, its high half the mask, so that who has the table has both.
// A string is looked for at startInStringTable(key), then 1, 2, 3 .. slots further on. The strings a function mentions have numbers in a row: the factor takes them apart.
static constexpr unsigned stringTableShift = 7;
static constexpr uint32_t startInStringTable(uint32_t key) { return key + (key << stringTableShift); }
static constexpr ptrdiff_t offsetOfMaskInStringTable = sizeof(uint32_t);

// An object of a Record or Instance layout:
//      0  JSCell
//      8  the order word, 32 bits: which of the orders of its layout its keys are in, how many properties of that order exist, flags
//     12  the shape word, 32 bits: the number of its layout among those that are allocated, negated
//     16  the fields
// Where the two words are, an ordinary object has its butterfly. Shapes are numbered from 1. So `word + last < count`, unsigned, in 32
// bits, holds exactly of the shapes first..last: the upper half of a pointer is a small number or zero, for which it does not hold
// (count <= last), so no other object passes for one of a shape. Nor does a string or a symbol, which have a pointer there, nor a bigint, which
// has its hash there: that has 31 bits, see computeHash() in JSBigInt.cpp.
//
// Compiled code reads the shape word only where a value is held to a type (Cast, and what tests its result), where an optional field is
// not in every member of a view, and to tell which method to call. An access of a field tests nothing: the type of the receiver is a
// fact. So an object stays as its layout says for as long as it lives. What would change that is refused, see AOTTypedObject.h.
static constexpr uint32_t shapeWordOf(uint32_t shape) { return 0 - shape; }
static constexpr uint32_t shapeOf(uint32_t shapeWord) { return 0 - shapeWord; }
static constexpr uint32_t offsetOfOrderWord = 8;
static constexpr uint32_t offsetOfShapeWord = 12;
static constexpr uint32_t offsetOfFirstField = 16;

// Out-of-line slots. An object of a layout with LayoutData::hasExtension has a pointer to its extension at offsetOfExtension, and its
// inline fields start behind it. The extension is a cell of its own:
//      0  JSCell
//      8  its size in bytes, 32 bits, this header included. It never changes.
//     16  boxed slots (Store::JSValue), zero where nothing has been stored: absent
// FieldData::offset of an out-of-line field is from the start of the extension.
// An extension grows by need: it is at least as large as the farthest slot that has ever been created in it needs, and a slot that
// does not lie within its size is absent. The pointer is never null. Until an out-of-line field is created it is the one empty
// extension of the instance, of size offsetOfFirstExtensionSlot, which is never written. An extension of its own belongs to one object.
// To make it larger the run time replaces it by a copy. Its size is that of the cell it got, so there may be room for more.
//   read:  extension = object[offsetOfExtension]; offset + 8 <= extension->size ? extension[offset] : absent. Zero: absent.
//   write: the slot reads absent: DidCreateField, which sees to it that the slot lies within the extension, then load the pointer
//          again. Then extension[offset] = value, with the barrier on the extension. A slot that reads present is stored to in place.
// The pointer changes whenever the run time creates a field. An extension never shrinks, and the pointer never goes back to the empty
// one, also when every out-of-line field has been deleted: a slot that was once within the size stays so.
static constexpr uint32_t offsetOfExtension = 16;
static constexpr uint32_t offsetOfExtensionSize = 8;
static constexpr uint32_t offsetOfFirstExtensionSlot = 16;
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
// `@name` of JavaScriptCore's and Bun's builtin code, spelled `\xFDname` by the front end: what BuiltinNames::lookUpPrivateName() finds.
inline bool isEnginePrivateName(std::span<const uint8_t> name) { return !name.empty() && name[0] == 0xfd; }

static constexpr uint32_t fieldIsOptional = 1;
// Only in FieldData::flags: the slot is in the extension.
static constexpr uint32_t fieldIsOutOfLine = 2;
static constexpr uint32_t fieldIsImmutable = 4;
static constexpr uint32_t fieldAllowsUndefined = 8;
static constexpr uint32_t fieldAllowsNull = 16;
// Its type is written down: what code that was not compiled with the type stores to it is coerced as by `as`.
static constexpr uint32_t fieldIsAnnotated = 32;
// Of an Instance: no constructor defines the field, see Format::FieldIsAbsentAtBirth. No allocation has it among its born fields, and the
// run time leaves it absent in an object it makes. The values in between are for the compiler alone: AOTTypedIR.h.
static constexpr uint32_t fieldIsAbsentAtBirth = 8192;
// See Format::FieldIsNeverSeenAbsent. If all that share the slot have it, the slot has no way to say "absent".
static constexpr uint32_t fieldIsNeverSeenAbsent = 32768;
// The same as it was read. Placement takes the flag above back where the slot needs its marker all the same, see dropFlagsThatDoNotHold(): no operation of the program finds the
// field absent for that. Compiled code and the run time both go by this one for what the program may do: mayInherit(), isBornUnderAnyPrototype(), State::makesObjectsWithDefinitelyInitializedFields().
static constexpr uint32_t fieldIsNeverSeenAbsentAsWritten = 262144;
// Only in FieldData::flags. The field lists one string, and compiled code takes an object of the layout to hold a string of that text there for as long as the byte
// Instance::typedAlteredLiterals[flags >> guardOfPinnedFieldShift] is clear. Whoever stores anything else, or takes the property away, sets the byte first. See Placement::pinLiterals().
static constexpr uint32_t fieldIsPinned = 131072;
static constexpr uint32_t guardOfPinnedFieldShift = 24;
static constexpr uint32_t numberOfGuardsOfPinnedFields = 256;

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
    // For error messages: the type's name (a string index, may be the empty string), and the offset in Data of its ViewFields.
    // The offset is 0 when shapeOfView is set: then the LayoutData lists the fields.
    uint32_t name;
    uint32_t fields;
    // Where the type is declared: a string, or UINT32_MAX, and the line. See Format::Shape.
    uint32_t declaredPath;
    uint32_t declaredLine;
};

// The properties an interface or type alias declares, when no object is allocated as that type. Only error messages read this.
struct ViewField {
    uint32_t name;
    // Offset in Data of the RuntimeType.
    uint32_t type;
    uint32_t isOptional;
    // The declared type as the checker prints it: a string, or UINT32_MAX.
    uint32_t typeText;
};

struct ViewFields {
    uint32_t count;
    std::span<const ViewField> span() const { return { reinterpret_cast<const ViewField*>(this + 1), count }; }
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
    // There is one RuntimeShapeTest per view. For each view of the type: how many bytes it lies from this record.
    const int32_t* distancesOfViews() const { return reinterpret_cast<const int32_t*>(this + 1); }
    const RuntimeShapeTest& view(uint32_t index) const { return *reinterpret_cast<const RuntimeShapeTest*>(reinterpret_cast<const uint8_t*>(this) + distancesOfViews()[index]); }
    const uint32_t* jsTypes() const { return reinterpret_cast<const uint32_t*>(distancesOfViews() + numberOfViews); }
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
static constexpr uint32_t firstArgumentField = 3;
// The state of a coroutine that has completed. 0: it has not started. Otherwise the number of the suspension point.
static constexpr int32_t completedState = -1;
// The resume function takes the frame, the resume mode and what the coroutine is resumed with. The mode of an async function: whether that
// is thrown, and this bit if MicrotaskQueue::drainImpl() has entered the function, draining the queue of the realm, with nothing left to do
// when it returns.
static constexpr int32_t isEnteredByJobLoop = 4;
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
    // The largest `size` among the layout and all that an object of it may be turned into where it is. What the run time makes has that much room.
    uint32_t sizeWithRoomToSpecialize;
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
    // Of a class: a ClassPrototype finds the one class object there is. A computed name may be an index, and a lookup by index does not come through its hooks.
    bool makesMembersWhenLookedAt;
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
    // See Typed::Layout::discriminant and isAbstract. The field by its number among fields(). noBase: the layout is in no family.
    uint32_t discriminant;
    bool isAbstract;
    // See offsetOfExtension. extensionSize: no out-of-line slot of this layout reaches beyond it. 0 if it has none.
    bool hasExtension;
    // Of a class. The class object of a class that extends something foreign is made at the declaration, and what its prototype inherits from is known there.
    bool hasPrototypeAheadOfClassObject;
    // Of a class: there is one class object, of it and of each class it extends. A computed name is evaluated at the declaration.
    bool makesClassObjectWhenChanged;
    uint32_t extensionSize;
    // For the collector: the offsets in the object of the slots that hold references. Three 32-bit counts, then that many 32-bit offsets of
    // boxed slots (Store::JSValue), of cell slots (Store::Cell), of 32-bit slots (Store::Cell32, Store::OptionalCell32), each run ascending. From the start of this LayoutData, negative:
    // the list comes before it. 0: the layout has none. Out-of-line slots are not listed, offsetOfExtension is, as a cell slot.
    int32_t referencesOffset;
    uint32_t numberOfFields;
    // The offset in Data of the 16-bit numbers of the fields, ascending by FieldData::name and then by number. 0: there are few, which are gone through.
    uint32_t fieldsByNameOffset;
    // For each field, like ViewField::typeText. Only error messages read this.
    uint32_t typeTextsOffset;
    // Of a record: what its type declares, no code of the program stores, so that there is no field for it, and that lists strings or numbers: a discriminant that only ever comes out of parsed data. The offset in Data of their
    // number and, for each, the name, the number of LiteralData and their offset. 0: none. See Coercion::distanceTo().
    uint32_t literalsWithoutFieldOffset;
    // As in RuntimeShapeTest.
    uint32_t declaredPath;
    uint32_t declaredLine;
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
    // A site may pick it by a discriminant that is no constant: see Instance::TypedAllocation::fieldTemplate.
    uint32_t usesFieldTemplate;
};

// The byte of a function at Data::functionKindsOffset: its Format::FunctionKind, and for a generic entrypoint what the function is
// declared as, which decides what its closures are as objects.
static constexpr uint8_t functionKindMask = 7;
static constexpr uint8_t functionIsAsync = 8;
static constexpr uint8_t functionIsGenerator = 16;
// A generic entrypoint, a coroutine ramp, the program's entry: stack traces leave its frames out.
static constexpr uint8_t functionIsHiddenInStackTraces = 32;
// A plain function that asks for `new.target`, and its generic entrypoint: see State::newTarget.
static constexpr uint8_t functionReadsNewTarget = 64;
// See Format::FunctionIsOfBuiltinModule. Also its generic entrypoint.
static constexpr uint8_t functionIsOfBuiltinModule = 128;
// Which of Instance::typedClosureStructureIDs a closure of the function has.
// Format::FunctionKind::Constructor.
static constexpr uint8_t functionKindOfConstructors = 3;
static constexpr unsigned closureStructureOfClassObjects = 4;
static constexpr unsigned closureStructureOf(uint8_t kindAndWhatItIsDeclaredAs)
{
    if ((kindAndWhatItIsDeclaredAs & functionKindMask) == functionKindOfConstructors)
        return closureStructureOfClassObjects;
    return kindAndWhatItIsDeclaredAs / functionIsAsync & 3;
}

// Where in the source an assertion stands. Only audit builds pass it: an audit line is printed without a stack walk, and most of the
// operations that print one are not call sites. Errors that are thrown get their position from the stack trace.
struct AuditSite {
    static constexpr unsigned columnBits = 12;
    static constexpr uint32_t maxColumn = (1u << columnBits) - 1;
    static constexpr uint32_t pack(uint32_t line, uint32_t column) { return line << columnBits | (column < maxColumn ? column : maxColumn); }

    uint32_t function { UINT32_MAX };
    uint32_t lineAndColumn { 0 };
};

// Where the value of a free name is while the global object is as it was. All zero: nothing is known.
struct GlobalPropertyCache {
    uint32_t structureID;
    uint32_t globalLexicalBindingEpoch;
    // Negative: from the butterfly of the global object. Else the address of a variable.
    int64_t location;
    // Not empty: the name has this one value for good, whatever the words above say.
    EncodedJSValue value;
};

// What the program needs at run time: in the program data, read in place.
struct Data {
    static constexpr uint32_t expectedMagic = 0x44505942;

    uint32_t magic;
    uint32_t numberOfStrings;
    uint32_t stringsOffset; // Pairs of offset and length.
    // A bit per string: some operation compares it or takes it for a key.
    uint32_t atomStringsOffset;
    // The constants of the program that are not ASCII, as the engine holds them. Threes, in the order of the first: the number of the string, the offset of its characters, their number, with isDecodedTo16Bits.
    uint32_t numberOfDecodedStrings;
    uint32_t decodedStringsOffset;
    static constexpr uint32_t isDecodedTo16Bits = 1u << 31;
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
    // Three words each, sorted: the general entry of a Format::FunctionKind::Bound, that of the function it binds, how many arguments it binds.
    uint32_t numberOfBoundFunctions;
    uint32_t boundFunctionsOffset;
    // By EngineClass::index, in bytes: whether the front end has seen a class of the program extend the class.
    uint32_t extendedClassesOffset;
    // The bits of the tests that are no range, in 32-bit words: bit n is bit n % 32 of word n / 32.
    uint32_t shapeSetsOffset;
    uint32_t numberOfShapeSetWords;
    uint32_t sizeOfStatics;
    // What the statics hold before any code runs, where that is not zero bits. Four words each: the first 64-bit word, how many, the low and
    // the high half of what each of them holds.
    uint32_t numberOfStaticsRuns;
    uint32_t staticsRunsOffset;
    // In the statics, behind the variables.
    uint32_t numberOfGlobalPropertyCaches;
    uint32_t globalPropertyCachesOffset;
    uint32_t numberOfRoots;
    uint32_t rootsOffset;
    uint32_t numberOfFunctions;
    uint32_t functionsOffset; // For each, the number of the string that is its name.
    uint32_t functionKindsOffset; // For each a byte: its Format::FunctionKind, with functionIsAsync and functionIsGenerator.
    uint32_t functionLengthsOffset; // For each, its `length` property, in 16 bits.
    uint32_t closureCountsOffset; // For each two bytes: of its closures, how many words the collector follows, which come first, and how many words there are.
    // Ascending: the modules in whose place an entry function runs, by ProgramModule::entryOffset.
    uint32_t numberOfEntryPoints;
    uint32_t entryPointsOffset;
    // Pairs of the number of a module in the host's registry and the generic entrypoint of the function that is the module, in the order of the former.
    uint32_t numberOfBuiltinModules;
    uint32_t builtinModulesOffset;
    // Triples: the number the engine or the host has for a function it implements in JavaScript, the generic entrypoint of the function, its
    // ImplementationVisibility. In the order of the first.
    uint32_t numberOfBuiltinFunctions;
    uint32_t builtinFunctionsOffset;
    // Pairs of the generic entrypoint of a generator function and the function that runs its body, in the order of the former. With generatorTakesItsClosure: see Typed::Function::takesItsClosure.
    static constexpr uint32_t generatorTakesItsClosure = 1u << 31;
    uint32_t numberOfGeneratorFunctions;
    uint32_t generatorFunctionsOffset;
    // Pairs of the generic entrypoint of a plain function that declares `this` to be of a layout, and the number of the allocation that
    // [[Construct]] makes the object with, in the order of the former. The prototype is for whoever constructs to say.
    uint32_t numberOfConstructorFunctions;
    uint32_t constructorFunctionsOffset;
    // Pairs of the generic entrypoint of such a function and a shape, ascending: an object of the shape has a field without a way to say
    // "absent" that the function does not store first, so `new` does not hand the function the object it has made of that shape.
    uint32_t numberOfRefusedFreshObjects;
    uint32_t refusedFreshObjectsOffset;
    // Pairs of the generic entrypoint of a function and the number of properties that its text is seen to give to `this`, in the order of the
    // former: those that give it more than an ordinary object has room for as the engine makes it. See State::structureForThis().
    uint32_t numberOfPropertyCountsOfThis;
    uint32_t propertyCountsOfThisOffset;
    // Strings, ascending: the names of the fields of all object layouts, allocated or not, but for those the program is seen to define
    // on a `prototype` object (Format::Header::prototypePropertyNames). See Typed::PrototypeObject.
    uint32_t numberOfReservedFieldNames;
    uint32_t reservedFieldNamesOffset;
    // The names of the fields of all object layouts, but for `#name` and `@name`: pairs of StringImpl::hash() of the text and the string, ascending. Under UINT32_MAX, which is no hash: a text that is no valid UTF-8.
    uint32_t numberOfFieldNames;
    uint32_t fieldNamesOffset;
    // The program was compiled with Options::auditAOTTypedAssertions(): compiled code has room for a value that fails a test, and the run
    // time lets it go on. See State::assertionFailed().
    uint32_t isCompiledForAudit;
    // The program was compiled with Options::countAOTTypedAllocations(): every allocation calls the run time, which counts.
    uint32_t countsAllocations;
    // The program was compiled with Options::useAOTWideReferences(): no slot is a Store::Cell32 or Store::OptionalCell32, and the MarkedBlockCage is not needed.
    uint32_t usesWideReferences;
    // Pairs of a function and the function it belongs to, sorted by the first: a generic entrypoint and what it wraps, the function that
    // runs a coroutine's body and the coroutine. For matching a stack frame against a closure.
    uint32_t numberOfFunctionAliases;
    uint32_t functionAliasesOffset;
    // Only with isCompiledForAudit: for each function the string that is its source path, for the audit's lines.
    uint32_t functionSourcesOffset;
    // The functions that make objects of a layout where they are called with `new`, in order: the number of the generic entrypoint << 2, with what State::mayAlterPrototypeObjectOf() and makesObjectsWithDefinitelyInitializedFields() say.
    uint32_t numberOfMakers;
    uint32_t makersOffset;
    static constexpr uint32_t makesObjectsOfFlaggedLayout = 1;
    static constexpr uint32_t makesObjectsWithDefinitelyInitializedFields = 2;
    // Pairs, in order: AllocationData::constructedBy, the number of the allocation.
    uint32_t numberOfAllocationsByConstructor;
    uint32_t allocationsByConstructorOffset;
    // A word per string: what is asked of its characters when a name or a constant is made of them.
    uint32_t stringFactsOffset;
    static constexpr uint32_t stringHashMask = (1u << 24) - 1;
    static constexpr uint32_t stringIsASCII = 1u << 24;
    static constexpr uint32_t stringIsPrivateName = 1u << 25;
    static constexpr uint32_t stringIsEnginePrivateName = 1u << 26;

    template<typename T> const T* at(uint32_t offset) const { return reinterpret_cast<const T*>(reinterpret_cast<const uint8_t*>(this) + offset); }
    bool isEntryPoint(uint32_t entryOffsetOfModule) const { return std::ranges::binary_search(std::span { at<uint32_t>(entryPointsOffset), numberOfEntryPoints }, entryOffsetOfModule); }
    std::span<const uint8_t> string(uint32_t index) const
    {
        const uint32_t* pair = at<uint32_t>(stringsOffset) + 2 * index;
        return { at<uint8_t>(pair[0]), pair[1] };
    }
    bool isAtomString(uint32_t index) const { return at<uint32_t>(atomStringsOffset)[index / 32] >> index % 32 & 1; }
    uint32_t stringFacts(uint32_t index) const { return at<uint32_t>(stringFactsOffset)[index]; }
};

// An environment has no shape word. The low 16 bits of its shape are in the two bytes of its header that say nothing about a cell that is no object, JSCell::m_indexingTypeAndMisc and
// m_flags; its Structure, which it shares with all environments of those 65,536 shapes, says the rest. THE CELL LOCK HAS ITS TWO BITS IN THE FIRST OF THOSE BYTES: nothing may lock such a cell.
static constexpr uint32_t offsetOfLowByteOfEnvironmentShape = 4;
static constexpr uint32_t offsetOfHighByteOfEnvironmentShape = 6;
inline const LayoutData& layoutOfEnvironment(const Data& data, uint32_t firstShape, const void* cell)
{
    auto* header = static_cast<const uint8_t*>(cell);
    uint32_t shape = firstShape + header[offsetOfLowByteOfEnvironmentShape] + (header[offsetOfHighByteOfEnvironmentShape] << 8);
    return *data.at<LayoutData>(data.at<uint32_t>(data.layoutsOffset)[shape]);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
