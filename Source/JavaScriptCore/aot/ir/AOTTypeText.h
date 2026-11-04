/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTTypeArena.h"
#include <optional>
#include <wtf/PrintStream.h>
#include <wtf/text/StringView.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT { namespace Typed {

// The text of a type of the engine. The texts of Code, of summaries, of facts and of plans all print and read types through these, and so does the printer of IR text.
// What printType() writes, parseType() reads back to the same id. The text depends on the content alone, never on where a record lies.
//
//   type       = "never" | part ("|" part)*            parts in the order written here, no blanks outside quotes and brackets
//   primitives   undefined null boolean true false number int i32 <integer> int[<min>, <max>] double whole fractional fractional[<min>, <max>] string symbol bigint absent unspecified
//                                                      fractional[0, 5]: TypeParts::boundsOfFractions. Beside `number`, `double`, `fractional` or `anything` the bounds say nothing and are dropped
//   literals     "text" with the escapes of IR text, in the order of their texts; a symbol as IR text writes a property key
//                                                      any key but a text key: @@iterator   symbol("d")@m1:2   Symbol.for("x")   @"x"   @#"x"   #"x"@L3
//   unknown      anything (all of JavaScript's primitives and any object)   object   function
//                L<n>   L(<layout>)   L(m<n>)          some object of the layout, nobody knows which. The numbered layouts come first
//                <Class>   <Class><type, type>         some object of the class, with what is written of its contents. Trailing `anything` is left out: Map<string>
//                ?<Seed>+<Seed>                        behind the last part of `unknown`: where it came from, in the order of UnknownSeed. Left out where there are no seeds
//                                                      a seed is its enumerator as C++ spells it: anything?CaughtException+UnseenCode
//   named        f<n>                                  the closure of the function
//                builtin(<name>)   builtin(<name>, m<n>)   result(<name>, <ordinal>)   host(<n>)
//                coroutine(f<n>)   rest(f<n>)   prototypeOf(f<n>)   constructed(f<n>)
//                namespace(m<n>)   instance(L<n>)   prototype(L<n>)
//                site(f<n>, v<n>)   site(f<n>, v<n>, <ordinal>)
//                escaped(L<n>)   escaped(<Class>)   escapedFunctions(<name>)   escapedWithoutShape(<MakerOfShapelessObject as a number>)
//                <named>@<instance>                    the copy of it for an instance of a summary: ObjectRecord::instance, ObjectTable::findObjectOfInstance()
//              in the order of ObjectKind, then of the numbers written
//   internal     env L<n>   opaque(<OpaqueKind>)       nameOf(OpaqueKind): opaque(IteratorState). Beside anything but `absent` and `unspecified` either is an error
//
//   L<n>         Wherever a layout is written, with any options, the number is the one IR text uses. Program::layouts holds the layouts of the wire, then those of built-ins and namespaces,
//                then the derived ones. A layout of a built-in is L(<layout>) and that of a namespace L(m<n>), and neither takes a number, so the derived layouts count on from the wire's.
//                No number moves when the table of built-ins gains a layout. Program::numberInTextOf() gives the number, Program::layoutNumberedInText() the layout that has it.
//   <layout>     BuiltinTable::tokenOf(BuiltinLayoutID)
//   <name>       BuiltinTable::tokenOf(BuiltinID) of the canonical id
//   <Class>      BuiltinTable::tokenOf(BuiltinClassID)
//
// A set of more than `mostMembersPrinted` named objects prints its first members and "...+<n>": f1|...+3. Such a text does not parse.
// Neither does "-", which stands for a null type or object, nor "<invalid <kind> <number>>", which stands for a string, a layout, a built-in or a class outside its table,
// and for any string where the reader has no program.

struct TypeTextOptions {
    static constexpr uint32_t everyMember = UINT32_MAX;

    uint32_t mostMembersPrinted { everyMember };
    bool printsSeeds { true };
    // For the printer of IR text, which writes a wire type as the front end prints it (src/aot/ir/print_names.rs in Bun). parseType() does not read such a text back: the parser of IR text does.
    // The unnamed layouts stand under the fewest layouts whose families (Program::declaredMembersOf()) make up the set, in ascending order. A name is a member of the set that is not derived
    // and whose family lies inside the set. The largest family is taken first, of equal ones the lowest number, and a family that brings no new member is not named.
    // A layout that no such family accounts for is printed by itself. A layout outside the set is never a name, so the text reads back to the same set but need not be the text as written.
    // The rest follows the front end too: `anything`, `object` and `function` stand in front of `absent`, `unspecified` and the literals (anything|absent), closures in front of the other
    // named objects and classes behind them all (f1|builtin(X)|Array), a class has all of its contents or none (Map<string, anything>), an opaque kind is a bare word in lower case (iteratorstate).
    // Functions are not folded: what the wire wrote as one function prints as every closure and built-in of its family.
    bool namesFamiliesOfLayouts { false };
};

void printType(PrintStream&, TypeReader, TypeID, TypeTextOptions = { });
// "unknown" for ObjectTable::unknown(), which is a member of no set.
void printObject(PrintStream&, TypeReader, ObjectID);
// "[L1, L2]", and "-" for the empty set.
void printLayoutSet(PrintStream&, TypeReader, LayoutSetID);
// "[f1, site(f0, v2)]", "[f1, ...+3]" where it is cut short, and "-" for the empty set.
void printObjectSet(PrintStream&, TypeReader, ObjectSetID, TypeTextOptions = { });

struct TypeInText {
    TypeReader types;
    TypeID type;

    void dump(PrintStream& out) const { printType(out, types, type); }
};

// Each reads the longest text of its kind that begins at `offset` and moves `offset` behind it. Nothing: `error` says what is wrong, and `offset` stands where.
// A name that the program lacks is an error. The arena needs a program: strings and keys are looked up, never added.
// A "|" that no part follows is left to the caller. Parts are read in any order, and seeds behind any part of `unknown` and behind a named object. Seeds in a type without `unknown` are an error.
// parseObject() reads "unknown", parseType() does not.
// parseLayoutSet() reads "-" and "[]" as the empty set. Where a type or an object may be null, the caller takes the "-" itself.
// `text` holds the bytes of the text as Latin-1, so that between quotes a character is a byte of the string.
std::optional<TypeID> parseType(TypeArena&, StringView text, unsigned& offset, String& error);
std::optional<ObjectID> parseObject(const TypeArena&, StringView text, unsigned& offset, String& error);
std::optional<LayoutSetID> parseLayoutSet(TypeArena&, StringView text, unsigned& offset, String& error);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
