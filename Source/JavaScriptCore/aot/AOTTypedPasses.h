/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTTypedIR.h"

namespace JSC {

class VM;

namespace AOT {

struct CompiledCode;
struct ImageKey;

namespace Typed {

// No pass finds out a type. A value a pass makes gets a type that is in the program already: that of the value it stands in for, of the
// place it is read from, of what the function called is declared to yield, or one of TypeTable's.
//
// No pass, and none of the questions below, goes by what is expected of a value (Value::prediction), nor makes one prediction out of
// others. A value that stands for another, or takes its number, takes what is expected of it along.
//
// What looks at one function at a time runs on all functions side by side, and reads of other functions only what was there before.

// From what was read to what is lowered. Yields what the verifier has found, which is for whoever wrote the pass or the front end.
Vector<String> optimize(Program&);

// Over the whole program, one after the other.

// The strings by their text, the layout of closures.
void prepare(Program&);
// Splits each async function and generator into a ramp and a resume function with a coroutine frame.
void splitCoroutines(Program&);
// For every function that is used as a value, every method and every constructor of a class: what someone calls who does not know the
// function. It holds the arguments to the types of the parameters (Cast: As where the type is written down, else Store) and calls the function.
void createGenericEntrypoints(Program&);
// Gives every NewObjectWithShape its Allocation, in Function::allocations. Of a record: the fields its initializing stores create, in
// that order, all of them born. Of anything else: every field, in the order of the layout, all of them born.
void assignAllocations(Program&);
// Program::viewsRetaggedTo, from the lists of specializations as they are given.
void findViewsRetaggedTo(Program&);
// After the functions have been optimized: Function::allocations become Program::allocations, in the order of the functions. Then come
// those the run time makes objects with: Function::allocationOfThis.
void linkAllocations(Program&);
// What a call of each function may change: Function::writes and writesShapeWords, and the same of a call that comes back. And what
// casts may: Program::castsMayRetag, layoutsCoercedThroughArrays.
void computeEffectSummaries(Program&);
// Slots, representations, offsets, shape numbers and tests, orders.
void place(Program&);
// What the program needs at run time.
Vector<uint8_t> encodeData(const Program&);

// One function at a time. `function` is a copy of the one in the program, which stays as it was.

// Every operand is of the type its operation needs, every argument of the type of the parameter, every value stored of the type of the
// place. Appends what is not.
void verify(const Program&, FunctionID, const Function&, Vector<String>& findings);
void cleanUp(const Program&, Function&);
// A call of a method, an accessor or a constructor through a view becomes a call of the function, or a choice among the functions by
// shape. So does an access to a static method or accessor on the class object of a class that is evaluated once, and one through `super`
// in such a class. Yields how much it changed.
unsigned resolveCalls(const Program&, Function&);
// By size. Yields how much it changed.
unsigned inlineCalls(const Program&, FunctionID, Function&);
// A cast whose operand is of the type goes. An object that is made, stored to and then cast to a specialization of its layout before
// anyone has seen it is made with that. Yields how much it changed.
unsigned simplifyCasts(const Program&, Function&);
// A catch block that only closes the iterator of an array and rethrows is removed. Yields how many were.
unsigned removeRedundantCatchBlocks(const Program&, Function&);
// What comes down to one entry of the table of built-ins says so from here on.
void resolveBuiltins(const Program&, Function&);

// Questions.

// Whether carrying the value out can throw. Where a handler watches, such a value ends its block.
bool mayThrow(const Program&, const Function&, const Value&);
// Whether IsInstanceOfClass may come to walk the prototype chain of its operand, where a proxy runs its trap. Otherwise the shape says it.
bool mayWalkPrototypeChain(const Program&, const Function&, const Value&);
// Whether the operation by itself makes sure that what it yields is of the type of the value. If not, the lowering tests.
bool vouchesForItsType(const Program&, const Function&, const Value&);
// What an object of the class finds under the name on its prototype chain, if that is a method or accessor of that kind which stays what
// it is. None also where the program is seen to replace it: then an object may have anything under the name, of its own or inherited.
FunctionID findMethod(const Program&, LayoutID, StringID, MethodKind);
struct FieldAccess {
    LayoutID layout;
    uint32_t field;
};
// The field an access by that name comes down to: the receiver is looked at as one type of the program, which has it.
std::optional<FieldAccess> fieldOf(const Program&, const Type& receiver, StringID name);
// Of a field of the view, from the linking of allocations on: whether an object of a member that has no field for it has nothing else
// to go by either, no accessor, no method, nothing on its prototype chain. Then a read of it there yields an extra property or
// `undefined` and a store makes an extra property. Otherwise the access is carried out the general way there, which may run any code.
bool onlyFieldsSupply(const Program&, LayoutID view, StringID name);
// Whether the value at `index` in the block is a store to a field that comes right after the allocation of the object, with only such
// stores in between: nothing has happened to the object yet.
bool isInitializingStore(const Program&, const Function&, const Block&, size_t index);
// Whether the object was made by this very run of the function, so that nobody who was running before can have looked into it.
bool isLocalAllocation(const Function&, ValueID);
// What carrying out a Cast may change, from the effect summaries on. A test changes nothing, and where it fails and that is the end
// nothing is changed either. As and Coerce go on: a plain object is copied (RuntimeState), arrays it leads to are rewritten (Heap), and
// where Coerce converts an object to a primitive its methods run (Heap).
Writes writesOfCast(const Program&, const Function&, const Value&);
// Of a Cast for the reason Coerce, with the type of its operand and the type after `as`: whether it may make anything of an object. If
// not, and if the operand is of the type of the cast, it is a way to look at the operand and no more.
bool coercionMayChangeObjects(const Program&, const Type& from, const Type& coercedTo);
// Whether a Cast to the type for the reason Flow goes on where the test fails: an object that was made as something the type extends is
// what a type guard or a discriminant says, and is turned into one of the type where it is. Nothing is copied or rewritten for that.
bool narrowingMayRetag(const Program&, const Type&);
// Whether it may turn the object into one of a specialization of its layout, where it is. See Function::writesShapeWords.
bool castWritesShapeWords(const Program&, const Function&, const Value&);
// What carrying out a CallBuiltin may change.
Writes writesOfBuiltinCall(const Program&, const Function&, const Value&);
// What the code changes that carries out the entry in place. Nothing: there is no such code for these arguments, and the function of the
// engine is called.
std::optional<Writes> writesOfInlineBuiltin(InlineBuiltin, std::span<const Type> arguments);
// How GetByVal, PutByVal, IteratorOpen, IteratorHasNext, IteratorValue and IteratorClose are carried out, going by the given types: in
// place, for a receiver that is an array, a typed array or a string and a key that is a number. Then no code of the program runs, on
// any path, but for a store to an array that is no longer as it was made (JSObject::becomeDerivedArray()), which does all the language
// says. Generic: the way the language says, whatever the receiver is.
enum class KeyedAccess : uint8_t {
    Generic,
    Array,
    TypedArray,
    String,
};
KeyedAccess keyedAccessOf(const Program&, const Function&, const Value&);
// A number, or `undefined` or `null`, which operations on numbers take for numbers. Arithmetic on such operands is carried out in place.
bool isNumericOperand(const Type&);
// What an arithmetic operation or a comparison may change, by the types of its operands. The second is null for a unary operation.
Writes writesOfArithmetic(const Type&, const Type*);
Type elementTypeOf(TypedArrayType);
// The entry among those of one built-in that holds for these arguments.
BuiltinID applicableEntry(BuiltinID first, std::span<const Type> arguments);

// How a function hands back what it yields. Void: that is only ever `undefined`.
Rep resultRepOf(const Program&, FunctionID);

ImageKey keyOf(FunctionID);
bool compile(VM&, const Program&, FunctionID, CompiledCode&);
// What the map of the image (Options::aotMapFilePath) says of each function, separated by tabs: its name, after that of its class; its
// entrypoint; its number; the path of its module; its position there. Entrypoints: typed or untyped (written in TypeScript or not), ramp,
// resume, module, entry, and with "generic-" before it the generic entrypoint of such a function.
Vector<String> describeForMap(const Program&);

// For tests. Stages: "builtins" (the table, whatever the program), "read", "optimized". False, with the reason printed: the program cannot be read.
JS_EXPORT_PRIVATE bool dumpAtStage(std::span<const uint8_t>, const String& stage, PrintStream&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
