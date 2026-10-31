/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCompileOptionWords.h"
#include "AOTIR.h"
#include "AOTNativeTest.h"
#include "AOTProgramFile.h"
#include "AOTVector.h"
#include "AOTWire.h"
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <wtf/StdLibExtras.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/StringView.h>

namespace JSC { namespace AOT {

inline constexpr Typed::FunctionID readerFixtureStreamFunction { 7u };
inline constexpr Typed::FunctionID readerFixtureHandlerFunction { 8u };

inline constexpr ASCIILiteral readerFixtureText = R"IR(typesite w0 module=m0 position=3 kind=Assertion text="T"
expectedresult x0 type=L0|L2|L3 layouts=[L0]
layout L0 name="I" module=m0 fields=[{key="x" declared={type=i32 site=w0 flags=[IsExpected]} flags=[IsRequired]}]
layout L1 kind=Environment fields=[{key="c" declared={type=anything}}]
layout L2 kind=Instance name="C" module=m0 constructor=f1 fields=[{key="x" declared={type=number} flags=[IsRequired] originLayout=L0 originField=0}, {key=#"p"@L2 declared={type=anything} flags=[IsPrivate]}] staticFields=[{key="s" variable=g2}] supertypes=[L0]
layout L3 kind=Instance name="D" module=m0 base=L2 constructor=f3 supertypes=[L0, L2] flags=[HasHeritage]
variable g0 name="g" module=m0 declared={type=undefined|"a"|"b"} initialization=Undefined
variable g1 name="z" module=m1 declared={type=L0|L2|L3|Array<number>|Map<string, anything>}
variable g2 name="s" module=m0 declared={type=function} owner=L2
variable g3 name="t" module=m0 declared={type=builtin(GlobalThis)|builtin(MathMax)}
module m0 path="/a.ts" displayPath="a.ts" body=f0 requests=[{target=m1 completes=true}] exports=[{name="a" variable=g0}, {name="b" kind=Namespace module=m1}] requiredNames=[{module=m1 name="z"}] positions={lineStarts=[0, 12] columnAdjustments=[{offset=4 column=3}] textPrintedInPlace="\x01\x01\x023" textOfBody="\x06\x01" whereThisIsMade=[0, 2]} functionText="a => a\x0a" typesCutFromFunctionText="\x01\x08"
module m1 path="node:b" displayPath="node:b" kind=CommonJs flags=[IsLibrary] exportRule=CommonJs requireResult=ModuleExports syntheticExports=[{name="y" isKnownMember=true}, {name="z" variable=g1}]
modulefailure q0 importer=m0 specifier="./q"
ambiguousexport module=m0 name="c"
evalscope e0 mode=Sloppy flags=[IsInFunction] bindings=[{name="g" variable=g0}]
function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {
  b0():
    Return
}
function f1 name="C" module=m0 kind=BaseConstructor flags=[HasNewTarget] captures=[{type=env L1}] thisType={type=anything} results=[{type=anything}]
function f2 name="m" module=m0 kind=Method flags=[HasThis] captures=[{type=env L1}] thisType={type=anything} results=[{type=anything}]
function f3 name="D" module=m0 kind=DerivedConstructor flags=[HasNewTarget] captures=[{type=env L1}] thisType={type=anything} results=[{type=anything}]
function f4 module=m0 kind=CommonJsModule thisType={type=anything} results=[{type=anything}]
function f5 name="r0" module=m0 role=Library enclosingRuntimeFunction=PromiseConstructorPromise thisType={type=anything} results=[{type=anything}]
function f6 name="r1" module=m0 role=Library enclosingRuntimeFunction=DisposableStackPrototypeAddDisposableResource thisType={type=anything} results=[{type=anything}]
function f7 name="stream" module=m0 position=9 text={end=6} length=2 thisType={type=anything} parameters=[{type=anything}, {type=boolean}] results=[{type=anything}] {
  b0(v0: anything, v1: boolean):
    v2 = Integer integer=-5 @1002
    v3 = Double number=1.5 @1003
    v4 = Call v0, -, v2, v3 expectedResult=x0 [isAnnotatedPure] @1004
    v5 = Call v0, v2 calleeKey="k" calleeName="o.k" @1005
    v6 = NewObject -, v3 layout=L0 fields=[0] @1006
    SuperSet v6, v2, v0, v3 mode=Sloppy @1007
    v8 = NewTemplateObject templateStrings=[{cooked="a" raw="a"}, {raw="b"}] @1008
    v9 = PropGet v0 key="x" @1009
    v10 = Convert v9 declared={type=number flags=[IsExpected]} @1010
    v11 = Builtin builtin=GlobalThis @1011
    v12 = PropGet v11 key="g" [isFreeName] @1012
    v13 = TypeTest v0 testedType=number @1013
    v14 = Data data=d0 @1014
    Branch v1 -> taken b1(), notTaken b2() @2000
  b1():
    Switch v0 cases=[1, 2] -> case b3(v2), case b3(v2), otherwise b2() @2001
  b2() frequency=Unlikely:
    SwitchString v0 strings=["a"] -> case b3(v2), otherwise b3(v2) @2002
  b3(v15):
    v16 = NewRegExp pattern="a" regExpFlags=[Global] @1016
    Return v0 @2003
}
function f8 name="handled" module=m0 thisType={type=anything} parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = TypeTest v0 testedType=number @3001
    Branch v1 -> taken b1(| 0), notTaken b2() @4000
  b1(v2) handler=b3(v0):
    v3 = Call v0, -, v2 @3003
    Jump -> next b2() @4001
  b2():
    Return v0 @4002
  b3(v4) role=CatchHandler:
    v5 = CatchValue @3005
    Throw v5 @4003
}
data d0 root=Object{layout=L0 members=[{key="x" value=Number{value=1}}, {key="y" value=Array{contents=number elements=[Number{value=2}, Hole]}}]} module=m0
entry module=m0 hostReads=[{exportName="a"}]
runtimefunction id=PromiseConstructorPromise function=f5
runtimefunction id=DisposableStackPrototypeAddDisposableResource function=f6
webassemblymodule module=m0 bytes="\x00asm"
replaceablebuiltin member=MathMin
openedname holder=ObjectPrototype name="added"
)IR"_s;

// A constructor function whose objects get a layout by their tag.
inline constexpr ASCIILiteral readerFixtureWithTagsText = R"IR(module m0 path="main.ts" displayPath="main.ts" body=f0
entry module=m0
layout L0 kind=Instance name="NodeObject" constructor=f1 fields=[{key="kind" declared={type=undefined|null|string}}] supertypes=[L1] flags=[IsConstructorFunction]
layout L1 name="Node" fields=[{key="kind" declared={type=undefined|null|string} flags=[IsRequired]}]
layout L2 name="Add" fields=[{key="kind" declared={type=undefined|null|"add"} flags=[IsRequired]}, {key="left" declared={type=undefined|null|L1} flags=[IsRequired]}] supertypes=[L1]
layout L3 name="Num" fields=[{key="kind" declared={type=undefined|null|"num"} flags=[IsRequired]}, {key="value" declared={type=number} flags=[IsRequired]}] supertypes=[L1]
function f0 name="main" kind=Module results=[{type=anything}] {
    Return
}
function f1 name="NodeObject" flags=[HasThis] thisType={type=undefined|null|L1} parameters=[{type=undefined|null|string}] results=[{type=anything}] {
    PropSet v0, v1 key="kind" mode=Strict
    Return
}
)IR"_s;

inline uint32_t wordAt(std::span<const uint8_t> bytes, size_t offset)
{
    uint32_t word;
    memcpySpan(asMutableByteSpan(word), bytes.subspan(offset, sizeof(word)));
    return word;
}

inline void setWordAt(Vector<uint8_t>& bytes, size_t offset, uint32_t word)
{
    memcpySpan(bytes.mutableSpan().subspan(offset, sizeof(word)), asByteSpan(word));
}

struct ReaderFixture {
    explicit ReaderFixture(StringView text = readerFixtureText)
        : build(makeDefaultCompileOptions())
        , bytes(Typed::encodeProgramText(text, build))
    {
        AOT_TEST_CHECK(bytes.size() >= sizeof(Wire::Header));
    }

    std::unique_ptr<Typed::Program> read()
    {
        auto program = Typed::Program::read(bytes.span(), build);
        AOT_TEST_CHECK(program);
        return program;
    }

    static constexpr size_t offsetOfSection(Typed::ProgramList list, size_t offsetInSection = 0)
    {
        return offsetof(Wire::Header, sections) + static_cast<size_t>(list) * sizeof(Wire::Section) + offsetInSection;
    }

    uint32_t countOf(Typed::ProgramList list) const { return wordAt(bytes.span(), offsetOfSection(list, offsetof(Wire::Section, count))); }

    template<typename Record>
    size_t offsetOf(Typed::ProgramList list, uint32_t index, size_t offsetInRecord = 0) const
    {
        AOT_TEST_CHECK(index < countOf(list));
        return wordAt(bytes.span(), offsetOfSection(list, offsetof(Wire::Section, offsetInBytes))) + index * sizeof(Record) + offsetInRecord;
    }

    uint32_t word(size_t offset) const { return wordAt(bytes.span(), offset); }
    Wire::Span spanAt(size_t offset) const { return { word(offset + offsetof(Wire::Span, start)), word(offset + offsetof(Wire::Span, count)) }; }

    size_t offsetOfHeadAt(Typed::FunctionID function, uint32_t position) const
    {
        size_t body = offsetOf<Wire::Function>(Typed::ProgramList::Functions, function.index(), offsetof(Wire::Function, body));
        Wire::Span code = spanAt(body + offsetof(Wire::Body, code));
        std::optional<size_t> found;
        for (uint32_t index = code.start + 1; index < code.start + code.count; ++index) {
            size_t offset = offsetOf<uint32_t>(Typed::ProgramList::Code, index);
            if (word(offset) != position)
                continue;
            AOT_TEST_CHECK(!found);
            found = offset - sizeof(uint32_t);
        }
        AOT_TEST_CHECK(found);
        return *found;
    }

    BuildContext build;
    Vector<uint8_t> bytes;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
