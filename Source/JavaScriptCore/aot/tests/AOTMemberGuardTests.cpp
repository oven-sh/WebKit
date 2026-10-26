/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTClassPrototype.h"
#include "AOTClassTable.h"
#include "AOTClosure.h"
#include "AOTImage.h"
#include "AOTImageLoading.h"
#include "AOTImageTestSupport.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTModules.h"
#include "AOTObjectModel.h"
#include "AOTObjectModelCaches.h"
#include "AOTProgramIdentifiers.h"
#include "AOTProgramStatics.h"
#include "AOTRuntimeCounters.h"
#include "AOTStaticFields.h"
#include "AOTVector.h"
#include "JSCInlines.h"
#include "MarkedBlockCage.h"
#include "Options.h"
#include "PropertyNameArray.h"
#include "TopExceptionScope.h"
#include <array>
#include <optional>
#include <wtf/DataLog.h>
#include <wtf/FileSystem.h>
#include <wtf/NeverDestroyed.h>

#if OS(WINDOWS)
#include <windows.h>
#else
#include <fcntl.h>
#endif

namespace JSC { namespace AOT {

std::span<const NativeTest> memberGuardTests();

static constexpr ASCIILiteral programWithOpenClassesText = R"IR(module m0 path="/main.js" displayPath="main.js" body=f0
entry module=m0
layout L0 kind=Instance name="Base" module=m0 constructor=f1 fields=[{key="value" declared={type=number} flags=[IsRequired]}] staticFields=[{key="tally" variable=g0}, {key="mode" variable=g1}]
layout L1 kind=Instance name="Middle" module=m0 base=L0 constructor=f2 supertypes=[L0] flags=[HasHeritage]
layout L2 kind=Instance name="Leaf" module=m0 base=L1 constructor=f3 supertypes=[L0, L1] flags=[HasHeritage]
layout L3 kind=Instance name="Late" module=m0 base=L0 constructor=f14 supertypes=[L0] flags=[HasHeritage]
layout L4 kind=Instance name="Foreign" module=m0 constructor=f16 flags=[HasHeritage]
variable g0 name="tally" module=m0 declared={type=number} owner=L0
variable g1 name="mode" module=m0 declared={type="red"|"green"} owner=L0
variable g2 name="Base" module=m0 declared={type=anything} initialization=Undefined
variable g3 name="Middle" module=m0 declared={type=anything} initialization=Undefined
variable g4 name="Leaf" module=m0 declared={type=anything} initialization=Undefined
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="publish" [isFreeName]
    v2 = NewClosure function=f1
    v3 = NewClass v2 layout=L0
    v4 = NewClosure function=f4
    DefineMethod v3, v4 layout=L0 key="firstStep"
    v6 = NewClosure function=f5
    DefineMethod v3, v6 layout=L0 index=1 key="secondStep"
    v8 = NewClosure function=f6
    DefineMethod v3, v8 layout=L0 index=2 key="unusedStep"
    v10 = NewClosure function=f7
    DefineMethod v3, v10 layout=L0 side=Static index=3 key="createOne"
    v12 = NewClosure function=f13
    DefineMethod v3, v12 layout=L0 side=Static methodKind=Setter index=4 key="level"
    v14 = FinishClass v3 layout=L0
    v15 = CallDirect v14 function=f8
    v16 = ResolveHeritage v14
    v17 = NewClosure function=f2
    v18 = NewClass v17, v14, v16 layout=L1
    v19 = NewClosure function=f9
    DefineMethod v18, v19 layout=L1 key="middleStep"
    v21 = FinishClass v18 layout=L1
    v22 = ResolveHeritage v21
    v23 = NewClosure function=f3
    v24 = NewClass v23, v21, v22 layout=L2
    v25 = NewClosure function=f10
    DefineMethod v24, v25 layout=L2 key="leafStep"
    v27 = FinishClass v24 layout=L2
    PutModuleVariable v14 variable=g2
    PutModuleVariable v21 variable=g3
    PutModuleVariable v27 variable=g4
    v31 = NewClosure function=f11
    v32 = NewClosure function=f12
    v33 = NewClosure function=f15
    v34 = NewClosure function=f17
    v35 = Call v1, -, v14, v21, v27, v31, v32, v33, v34
    Return
}
function f1 name="Base" kind=BaseConstructor flags=[HasNewTarget] parameters=[{type=number}] results=[{type=anything}] {
    v2 = GetConstructPrototype v0 layout=L0
    v3 = NewObject v2 layout=L0
    PropSet v3, v1 key="value"
    Return v3
}
function f2 name="Middle" kind=DerivedConstructor flags=[HasNewTarget, TakesCallee] parameters=[{type=number}] results=[{type=anything}] {
    v3 = GetSuperBase v0
    v4 = SuperConstruct v3, v1, v2 layout=L1
    Return v4
}
function f3 name="Leaf" kind=DerivedConstructor flags=[HasNewTarget, TakesCallee] parameters=[{type=number}] results=[{type=anything}] {
    v3 = GetSuperBase v0
    v4 = SuperConstruct v3, v1, v2 layout=L2
    Return v4
}
function f4 name="firstStep" kind=Method flags=[HasThis] results=[{type=anything}] {
    Return
}
function f5 name="secondStep" kind=Method flags=[HasThis] results=[{type=anything}] {
    Return
}
function f6 name="unusedStep" kind=Method flags=[HasThis] results=[{type=anything}] {
    Return
}
function f7 name="createOne" kind=Method flags=[HasThis] results=[{type=anything}] {
    Return
}
function f8 name="initializer" kind=Initializer flags=[HasThis] thisType={type=function} results=[{type=anything}] {
    v1 = Integer integer=0
    PropDefine v0, v1 key="tally"
    v3 = String string="red"
    PropDefine v0, v3 key="mode"
    Return
}
function f9 name="middleStep" kind=Method flags=[HasThis] results=[{type=anything}] {
    Return
}
function f10 name="leafStep" kind=Method flags=[HasThis] results=[{type=anything}] {
    Return
}
function f11 name="use" parameters=[{type=number}] results=[{type=anything}] {
    v1 = GetModuleVariable variable=g2
    v2 = GetModuleVariable variable=g3
    v3 = GetModuleVariable variable=g4
    v4 = Construct v1, -, v0
    v5 = Construct v2, -, v0
    v6 = Construct v3, -, v0
    v7 = PropGet v4 key="firstStep"
    v8 = Call v7, v4 calleeKey="firstStep"
    v9 = PropGet v5 key="firstStep"
    v10 = Call v9, v5 calleeKey="firstStep"
    v11 = PropGet v6 key="firstStep"
    v12 = Call v11, v6 calleeKey="firstStep"
    v13 = PropGet v6 key="secondStep"
    v14 = Call v13, v6 calleeKey="secondStep"
    v15 = PropGet v6 key="middleStep"
    v16 = Call v15, v6 calleeKey="middleStep"
    v17 = PropGet v6 key="leafStep"
    v18 = Call v17, v6 calleeKey="leafStep"
    v19 = PropGet v1 key="createOne"
    v20 = Call v19, v1 calleeKey="createOne"
    v21 = PropGet v3 key="createOne"
    v22 = Call v21, v3 calleeKey="createOne"
    v23 = PropGet v1 key="tally"
    v24 = PropGet v3 key="tally"
    v25 = PropGet v1 key="mode"
    v26 = PropGet v3 key="mode"
    PropSet v1, v0 key="level"
    v28 = PropGet v3 key="level"
    v29 = PropGet v4 key="absentStep"
    v30 = PropGet v6 key="absentStep"
    v31 = PropGet v4 key="secondStep"
    v32 = Call v31, v4 calleeKey="secondStep"
    v33 = PropGet v5 key="secondStep"
    v34 = Call v33, v5 calleeKey="secondStep"
    v35 = NewArray v23, v24, v25, v26, v29, v30 contents=anything holes=[]
    Return v35
}
function f12 name="mutate" parameters=[{type=anything}, {type=string}, {type=anything}] results=[{type=anything}] {
    ElemSet v0, v1, v2
    v4 = ElemDelete v0, v1
    v5 = Builtin builtin=ObjectSetPrototypeOf
    v6 = Call v5, -, v0, v2
    Return
}
function f13 name="level" kind=Setter flags=[HasThis] parameters=[{type=anything}] results=[{type=anything}] {
    Return
}
function f14 name="Late" kind=DerivedConstructor flags=[HasNewTarget, TakesCallee] parameters=[{type=number}] results=[{type=anything}] {
    v3 = GetSuperBase v0
    v4 = SuperConstruct v3, v1, v2 layout=L3
    Return v4
}
function f15 name="defineLate" results=[{type=anything}] {
    v0 = GetModuleVariable variable=g2
    v1 = ResolveHeritage v0
    v2 = NewClosure function=f14
    v3 = NewClass v2, v0, v1 layout=L3
    v4 = FinishClass v3 layout=L3
    v5 = PropGet v4 key="tally"
    v6 = PropGet v4 key="createOne"
    v7 = Call v6, v4 calleeKey="createOne"
    v8 = NewArray v4, v5 contents=anything holes=[]
    Return v8
}
function f16 name="Foreign" kind=DerivedConstructor flags=[HasNewTarget, TakesCallee] results=[{type=anything}] {
    v2 = GetSuperBase v0
    v3 = SuperConstruct v2, v1 layout=L4
    Return v3
}
function f17 name="defineForeign" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = ResolveHeritage v0
    v2 = NewClosure function=f16
    v3 = NewClass v2, v0, v1 layout=L4
    v4 = NewClosure function=f18
    DefineMethod v3, v4 layout=L4 key="foreignStep"
    v6 = FinishClass v3 layout=L4
    v7 = Construct v6
    v8 = PropGet v7 key="foreignStep"
    v9 = Call v8, v7 calleeKey="foreignStep"
    v10 = PropGet v7 key="absentStep"
    v11 = NewArray v6, v10 contents=anything holes=[]
    Return v11
}
function f18 name="foreignStep" kind=Method flags=[HasThis] results=[{type=anything}] {
    Return
}
)IR"_s;

static constexpr ASCIILiteral programWithLazyClassText = R"IR(module m0 path="/main.js" displayPath="main.js" body=f0
entry module=m0
layout L0 kind=Instance name="Lazy" module=m0 constructor=f1
layout L1 kind=Instance name="Shadowing" module=m0 base=L0 constructor=f2 supertypes=[L0] flags=[HasHeritage]
variable g0 name="Shadowing" module=m0 declared={type=anything} initialization=Undefined
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="publish" [isFreeName]
    v2 = NewClosure function=f1
    v3 = NewClass v2 layout=L0
    v4 = NewClosure function=f3
    DefineMethod v3, v4 layout=L0 key="lazyStep"
    v6 = FinishClass v3 layout=L0
    v7 = ResolveHeritage v6
    v8 = NewClosure function=f2
    v9 = NewClass v8, v6, v7 layout=L1
    v10 = FinishClass v9 layout=L1
    PutModuleVariable v10 variable=g0
    v12 = NewClosure function=f4
    v13 = NewClosure function=f5
    v14 = Call v1, -, v12, v13
    Return
}
function f1 name="Lazy" kind=BaseConstructor flags=[HasNewTarget] results=[{type=anything}] {
    v1 = GetConstructPrototype v0 layout=L0
    v2 = NewObject v1 layout=L0
    Return v2
}
function f2 name="Shadowing" kind=DerivedConstructor flags=[HasNewTarget, TakesCallee] results=[{type=anything}] {
    v2 = GetSuperBase v0
    v3 = SuperConstruct v2, v1 layout=L1
    Return v3
}
function f3 name="lazyStep" kind=Method flags=[HasThis] results=[{type=anything}] {
    Return
}
function f4 name="useShadowing" results=[{type=anything}] {
    v0 = GetModuleVariable variable=g0
    v1 = Construct v0
    v2 = PropGet v1 key="lazyStep"
    v3 = Call v2, v1 calleeKey="lazyStep"
    Return
}
function f5 name="shadow" parameters=[{type=string}, {type=anything}] results=[{type=anything}] {
    v2 = GetModuleVariable variable=g0
    v3 = PropGet v2 key="prototype"
    ElemSet v3, v0, v1
    Return
}
)IR"_s;

static constexpr ASCIILiteral programWithComputedMemberText = R"IR(module m0 path="/main.js" displayPath="main.js" body=f0
entry module=m0
layout L0 kind=Instance name="Above" module=m0 constructor=f1 fields=[{key="x" declared={type=anything}}] flags=[IsConstructorFunction]
layout L1 kind=Instance name="Computed" module=m0 base=L0 constructor=f2 supertypes=[L0] flags=[HasHeritage]
variable g0 name="Computed" module=m0 declared={type=anything} initialization=Undefined
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="publish" [isFreeName]
    v2 = NewClosure function=f1
    v3 = PropGet v2 key="prototype"
    v4 = NewClosure function=f3
    PropSet v3, v4 key="step"
    v6 = Construct v2
    v7 = ResolveHeritage v2
    v8 = NewClosure function=f2
    v9 = NewClass v8, v2, v7 layout=L1
    v10 = PropGet v0 key="nameOfMember" [isFreeName]
    v11 = ToPropertyKey v10
    v12 = NewClosure function=f5
    DefineMethod v9, v12, v11 layout=L1
    v14 = FinishClass v9 layout=L1
    PutModuleVariable v14 variable=g0
    v16 = NewClosure function=f4
    v17 = Call v1, -, v16
    Return
}
function f1 name="Above" flags=[HasThis] thisType={type=L0 flags=[IsExpected]} results=[{type=anything}] {
  b0(v0: anything):
    v1 = Integer integer=1
    PropSet v0, v1 key="x"
    v3 = Undefined
    Return v3
}
function f2 name="Computed" kind=DerivedConstructor flags=[HasNewTarget, TakesCallee] results=[{type=anything}] {
    v2 = GetSuperBase v0
    v3 = SuperConstruct v2, v1 layout=L1
    Return v3
}
function f3 name="step" flags=[HasThis] results=[{type=anything}] {
    Return
}
function f4 name="useComputed" results=[{type=anything}] {
    v0 = GetModuleVariable variable=g0
    v1 = Construct v0
    v2 = PropGet v1 key="step"
    v3 = Call v2, v1 calleeKey="step"
    Return
}
function f5 kind=Method flags=[HasThis] results=[{type=anything}] {
    Return
}
)IR"_s;

static constexpr ASCIILiteral programWithClassDefinedLaterText = R"IR(module m0 path="/main.js" displayPath="main.js" body=f0
entry module=m0
layout L0 kind=Instance name="Early" module=m0 constructor=f1
layout L1 kind=Instance name="Late" module=m0 base=L0 constructor=f2 supertypes=[L0] flags=[HasHeritage]
variable g0 name="Early" module=m0 declared={type=anything} initialization=Undefined
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="publish" [isFreeName]
    v2 = NewClosure function=f1
    v3 = NewClass v2 layout=L0
    v4 = NewClosure function=f3
    DefineMethod v3, v4 layout=L0 key="step"
    v6 = FinishClass v3 layout=L0
    PutModuleVariable v6 variable=g0
    v8 = NewClosure function=f4
    v9 = NewClosure function=f5
    v10 = Call v1, -, v6, v8, v9
    Return
}
function f1 name="Early" kind=BaseConstructor flags=[HasNewTarget] results=[{type=anything}] {
    v1 = GetConstructPrototype v0 layout=L0
    v2 = NewObject v1 layout=L0
    Return v2
}
function f2 name="Late" kind=DerivedConstructor flags=[HasNewTarget, TakesCallee] results=[{type=anything}] {
    v2 = GetSuperBase v0
    v3 = SuperConstruct v2, v1 layout=L1
    Return v3
}
function f3 name="step" kind=Method flags=[HasThis] results=[{type=anything}] {
    v1 = Integer integer=7
    Return v1
}
function f4 name="callStep" parameters=[{type=L0|L1 flags=[IsExpected]}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = Check v0 expectedType=L0|L1
    v2 = PropGet v1 key="step"
    v3 = Call v2, v1 calleeKey="step"
    Return v3
}
function f5 name="defineLate" results=[{type=anything}] {
    v0 = GetModuleVariable variable=g0
    v1 = ResolveHeritage v0
    v2 = NewClosure function=f2
    v3 = NewClass v2, v0, v1 layout=L1
    v4 = FinishClass v3 layout=L1
    v5 = Construct v4
    Return v5
}
)IR"_s;

static constexpr ASCIILiteral programWithClassObjectNobodyReadsText = R"IR(module m0 path="/main.js" displayPath="main.js" body=f0
entry module=m0
layout L0 kind=Instance name="Quiet" module=m0 constructor=f1
variable g0 name="Quiet" module=m0 declared={type=anything} initialization=Undefined
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=Function
    v1 = String string="return globalThis.publish"
    v2 = Construct v0, -, v1
    v3 = Call v2
    v4 = NewClosure function=f1
    v5 = NewClass v4 layout=L0
    v6 = NewClosure function=f2
    DefineMethod v5, v6 layout=L0 key="step"
    v8 = FinishClass v5 layout=L0
    PutModuleVariable v8 variable=g0
    v10 = NewClosure function=f3
    v11 = NewClosure function=f4
    v12 = Call v3, -, v10, v11
    Return
}
function f1 name="Quiet" kind=BaseConstructor flags=[HasNewTarget] results=[{type=anything}] {
    v1 = GetConstructPrototype v0 layout=L0
    v2 = NewObject v1 layout=L0
    Return v2
}
function f2 name="step" kind=Method flags=[HasThis] results=[{type=anything}] {
    v1 = Integer integer=7
    Return v1
}
function f3 name="make" results=[{type=anything}] {
    v0 = GetModuleVariable variable=g0
    v1 = Construct v0
    Return v1
}
function f4 name="callStep" parameters=[{type=L0 flags=[IsExpected]}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = Check v0 expectedType=L0
    v2 = PropGet v1 key="step"
    v3 = Call v2, v1 calleeKey="step"
    Return v3
}
)IR"_s;

static intptr_t openForMappingCode(const char* path)
{
#if OS(WINDOWS)
    HANDLE file = CreateFileA(path, GENERIC_READ | GENERIC_EXECUTE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    AOT_TEST_CHECK(file != INVALID_HANDLE_VALUE);
    return reinterpret_cast<intptr_t>(file);
#else
    int descriptor = open(path, O_RDONLY);
    AOT_TEST_CHECK(descriptor >= 0);
    return descriptor;
#endif
}

static void useImageWithCodeMappedFromFile(const char* path)
{
    static NeverDestroyed<ImageTests::AlignedBytes> image;
    std::optional<Vector<uint8_t>> contents = FileSystem::readEntireFile(String::fromUTF8(path));
    AOT_TEST_CHECK(contents);
    image.get() = ImageTests::AlignedBytes(contents->span());
    constexpr std::array<uint8_t, 16> identityOfLoadedPrograms { };
    AOT_TEST_CHECK(useImage(image->span(), ImageFile { openForMappingCode(path), 0 }, identityOfLoadedPrograms) == Wire::ImageVerdict::Accepted);
    AOT_TEST_CHECK(Image::current()->header().options.useWideReferences() || MarkedBlockCage::require());
}

namespace {

struct Guard {
    bool isOn(ClassSide expectedSide, MemberGuardKind expectedKind) const { return side == expectedSide && kind == expectedKind; }
    bool isOnMember(ClassSide expectedSide, ASCIILiteral expectedName) const { return isOn(expectedSide, MemberGuardKind::Member) && name == expectedName; }
    bool saysNoInstanceHides(ASCIILiteral expectedName) const { return isOn(ClassSide::Prototype, MemberGuardKind::NotHidden) && name == expectedName; }
    bool isOfOneOf(std::initializer_list<ASCIILiteral> classNames) const
    {
        return std::ranges::any_of(classNames, [&](ASCIILiteral candidate) {
            return className == candidate;
        });
    }

    String className;
    ClassSide side;
    MemberGuardKind kind;
    String name;
    bool isSet;
    bool holdsFromStart;
};

using Guards = Vector<Guard>;

class RunningProgram {
    WTF_MAKE_NONCOPYABLE(RunningProgram);
public:
    explicit RunningProgram(LoadedProgram& program)
        : m_program(program)
    {
    }

    VM& vm() { return m_program.vm(); }
    JSGlobalObject* realm() { return m_program.realm(); }
    State& state() { return m_program.state(); }
    JSValue evaluate(ASCIILiteral source) { return m_program.realms().evaluate(realm(), source); }
    void evaluateModule(ASCIILiteral definitionOfPublish);
    const LayoutData& classNamed(ASCIILiteral);
    Guards readGuards();
    void checkNoSetOutlivesItsGuards();
    Vector<uint32_t> setsWith(ASCIILiteral className, MemberGuardKind, ASCIILiteral name);
    bool isSetNumbered(uint32_t set);
    bool isSet(ASCIILiteral className, ClassSide, MemberGuardKind, ASCIILiteral name = { });
    void checkClears(ASCIILiteral source, unsigned minimumNumber, const Invocable<bool(const Guard&)> auto& isExpectedToBeCleared);
    void checkClearsNothing(ASCIILiteral source);

private:
    void forEachClass(const Invocable<void(const LayoutData&)> auto&);

    LoadedProgram& m_program;
};

} // anonymous namespace

void RunningProgram::evaluateModule(ASCIILiteral definitionOfPublish)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm());
    evaluate(definitionOfPublish);
    std::optional<ModuleID> module = findModule("/main.js"_s);
    AOT_TEST_CHECK(module);
    evaluateModuleSynchronously(realm(), *module);
    AOT_TEST_CHECK(!scope.exception() && modulesOf(realm())->status(*module) == ModuleStatus::Evaluated);
}

void RunningProgram::forEachClass(const Invocable<void(const LayoutData&)> auto& functor)
{
    const ProgramTables& tables = state().tables;
    for (uint32_t shape = 1; shape < tables.firstEnvironmentShape; ++shape) {
        if (tables.hasLayoutForShape(shape) && tables.layoutForShape(shape).isClass())
            functor(tables.layoutForShape(shape));
    }
}

const LayoutData& RunningProgram::classNamed(ASCIILiteral name)
{
    const LayoutData* found = nullptr;
    forEachClass([&](const LayoutData& layout) {
        if (Image::current()->text().equals(StringID(layout.name), name))
            found = &layout;
    });
    AOT_TEST_CHECK_FOR_SOURCE(found, name);
    return *found;
}

Guards RunningProgram::readGuards()
{
    Guards guards;
    forEachClass([&](const LayoutData& layout) {
        String className = Image::current()->text().createString(StringID(layout.name));
        for (const MemberGuardData& guard : layout.memberGuards()) {
            bool hasName = guard.kind() == MemberGuardKind::Member || guard.kind() == MemberGuardKind::NotHidden || guard.kind() == MemberGuardKind::PassedByStores;
            String name = hasName ? ProgramIdentifiers::textOf(*Image::current(), guard.name) : String();
            bool isBitSet = state().objects->classes.guardWords(layout, guard.side())[guard.wordIndex()] & guard.mask();
            bool holdsFromStart = layout.classData().guardsHoldWhileClear;
            guards.append({ className, guard.side(), guard.kind(), name, isBitSet != holdsFromStart, holdsFromStart });
        }
    });
    return guards;
}

// A site with several conditions tests the one bit of their set.
void RunningProgram::checkNoSetOutlivesItsGuards()
{
    forEachClass([&](const LayoutData& layout) {
        for (const MemberGuardData& guard : layout.memberGuards()) {
            bool holdsFromStart = layout.classData().guardsHoldWhileClear;
            bool isBitSet = state().objects->classes.guardWords(layout, guard.side())[guard.wordIndex()] & guard.mask();
            if (isBitSet != holdsFromStart)
                continue;
            uint32_t guardWord = MemberGuardAggregates::wordIndex(MemberGuardAggregates::bitNumber(*layout.classData().guardWordsOffsetFor(guard.side()), guard.bit));
            if (!holdsFromStart && !state().objects->caches->memberGuardWatchpoints.isAboutExistingClass(guardWord))
                continue;
            MemberGuardAggregates aggregates = layout.aggregatesOf(guard);
            for (uint32_t set : std::span { state().tables.at<uint32_t>(aggregates.offset), aggregates.count })
                AOT_TEST_CHECK(!isSetNumbered(set));
        }
    });
}

Vector<uint32_t> RunningProgram::setsWith(ASCIILiteral className, MemberGuardKind kind, ASCIILiteral name)
{
    const LayoutData& layout = classNamed(className);
    for (const MemberGuardData& guard : layout.memberGuards()) {
        if (guard.side() != ClassSide::Prototype || guard.kind() != kind || ProgramIdentifiers::textOf(*Image::current(), guard.name) != name)
            continue;
        MemberGuardAggregates aggregates = layout.aggregatesOf(guard);
        return Vector<uint32_t>(std::span { state().tables.at<uint32_t>(aggregates.offset), aggregates.count });
    }
    failNativeTest(__FILE__, __LINE__, name.characters());
}

bool RunningProgram::isSetNumbered(uint32_t set)
{
    const uint64_t* words = reinterpret_cast<const uint64_t*>(state().instance.statics());
    uint32_t number = MemberGuardAggregates::numberInListEntry(set);
    bool isBitSet = words[MemberGuardAggregates::wordIndex(number)] & MemberGuardAggregates::mask(number);
    return isBitSet != !!(set & MemberGuardAggregates::holdsWhileClear);
}

bool RunningProgram::isSet(ASCIILiteral className, ClassSide side, MemberGuardKind kind, ASCIILiteral name)
{
    for (const Guard& guard : readGuards()) {
        if (guard.className == className && guard.isOn(side, kind) && (guard.name.isNull() || guard.name == name))
            return guard.isSet;
    }
    failNativeTest(__FILE__, __LINE__, name.isNull() ? className.characters() : name.characters());
}

void RunningProgram::checkClears(ASCIILiteral source, unsigned minimumNumber, const Invocable<bool(const Guard&)> auto& isExpectedToBeCleared)
{
    Guards before = readGuards();
    uint64_t membersBefore = valueOf(vm(), RuntimeCounterID::MemberGuardsCleared);
    uint64_t linksBefore = valueOf(vm(), RuntimeCounterID::PrototypeLinkGuardsCleared);
    evaluate(source);
    Guards after = readGuards();

    unsigned numberOfGuards = 0;
    unsigned numberOfMembers = 0;
    unsigned numberOfLinks = 0;
    for (size_t index = 0; index < before.size(); ++index) {
        bool isCleared = before[index].isSet && isExpectedToBeCleared(before[index]);
        numberOfGuards += isCleared;
        numberOfMembers += isCleared && before[index].kind == MemberGuardKind::Member;
        numberOfLinks += isCleared && before[index].kind == MemberGuardKind::PrototypeLink;
        if (after[index].isSet != (before[index].isSet && !isCleared)) {
            dataLogLn(before[index].className, " side ", static_cast<unsigned>(before[index].side), " kind ", static_cast<unsigned>(before[index].kind), " ", before[index].name, " is ", after[index].isSet ? "set" : "clear");
            failNativeTest(__FILE__, __LINE__, source.characters());
        }
    }
    AOT_TEST_CHECK_FOR_SOURCE(numberOfGuards >= minimumNumber, source);
    AOT_TEST_CHECK_FOR_SOURCE(valueOf(vm(), RuntimeCounterID::MemberGuardsCleared) == membersBefore + numberOfMembers, source);
    AOT_TEST_CHECK_FOR_SOURCE(valueOf(vm(), RuntimeCounterID::PrototypeLinkGuardsCleared) == linksBefore + numberOfLinks, source);
    checkNoSetOutlivesItsGuards();
}

void RunningProgram::checkClearsNothing(ASCIILiteral source)
{
    checkClears(source, 0, [](const Guard&) {
        return false;
    });
}

static constexpr ASCIILiteral publishOpenClasses = "globalThis.publish = function (Base, Middle, Leaf, use, mutate, defineLate, defineForeign) { globalThis.Base = Base; globalThis.Middle = Middle; globalThis.Leaf = Leaf; globalThis.use = use; globalThis.defineLate = defineLate; globalThis.defineForeign = defineForeign }"_s;
static constexpr ASCIILiteral publishClassDefinedLater = "globalThis.publish = function (Early, callStep, defineLate) { globalThis.Early = Early; globalThis.callStep = callStep; globalThis.defineLate = defineLate }"_s;
static constexpr ASCIILiteral publishClassObjectNobodyReads = "globalThis.publish = function (make, callStep) { globalThis.make = make; globalThis.callStep = callStep }"_s;
static constexpr ASCIILiteral publishLazyClass = "globalThis.publish = function (useShadowing, shadow) { globalThis.useShadowing = useShadowing; globalThis.shadow = shadow }"_s;

static void checkGuardsAreSetAtTheDefinition(LoadedProgram& loaded)
{
    RunningProgram program(loaded);
    AOT_TEST_CHECK(program.readGuards().size() >= 20);
    for (const Guard& guard : program.readGuards()) {
        AOT_TEST_CHECK(guard.isSet == guard.holdsFromStart);
        AOT_TEST_CHECK(guard.holdsFromStart);
    }

    program.evaluateModule(publishOpenClasses);
    for (const Guard& guard : program.readGuards())
        AOT_TEST_CHECK(guard.isSet);
    AOT_TEST_CHECK(program.isSet("Base"_s, ClassSide::Prototype, MemberGuardKind::Member, "firstStep"_s) && program.isSet("Leaf"_s, ClassSide::Prototype, MemberGuardKind::Member, "firstStep"_s));
    AOT_TEST_CHECK(program.isSet("Leaf"_s, ClassSide::Static, MemberGuardKind::Member, "createOne"_s) && program.isSet("Leaf"_s, ClassSide::Static, MemberGuardKind::Member, "tally"_s));
    AOT_TEST_CHECK(program.isSet("Middle"_s, ClassSide::Prototype, MemberGuardKind::PrototypeLink) && program.isSet("Middle"_s, ClassSide::Static, MemberGuardKind::PrototypeLink));

    program.evaluate("defineLate()"_s);
    for (const Guard& guard : program.readGuards())
        AOT_TEST_CHECK(guard.isSet);
    AOT_TEST_CHECK(program.isSet("Late"_s, ClassSide::Static, MemberGuardKind::EvaluatedOnce) && program.isSet("Late"_s, ClassSide::Static, MemberGuardKind::Member, "tally"_s));

    program.evaluate("defineForeign(function () { })"_s);
    AOT_TEST_CHECK(program.classNamed("Foreign"_s).hasUntypedBase);
    for (const Guard& guard : program.readGuards())
        AOT_TEST_CHECK(guard.isSet);
    AOT_TEST_CHECK(program.isSet("Foreign"_s, ClassSide::Prototype, MemberGuardKind::Member, "foreignStep"_s));
    AOT_TEST_CHECK(!valueOf(program.vm(), RuntimeCounterID::MemberGuardsCleared) && !valueOf(program.vm(), RuntimeCounterID::PrototypeLinkGuardsCleared) && !valueOf(program.vm(), RuntimeCounterID::ClassesEvaluatedAgain));
}

static void checkChangedMemberOfBaseClass(LoadedProgram& loaded)
{
    RunningProgram program(loaded);
    program.evaluateModule(publishOpenClasses);
    program.checkClears("Base.prototype.firstStep = function () { }"_s, 3, [](const Guard& guard) {
        return guard.isOnMember(ClassSide::Prototype, "firstStep"_s);
    });
    program.checkClearsNothing("Base.prototype.firstStep = function () { }"_s);
    program.checkClears("delete Base.prototype.secondStep"_s, 3, [](const Guard& guard) {
        return guard.isOnMember(ClassSide::Prototype, "secondStep"_s);
    });
    program.checkClears("Object.defineProperty(Leaf.prototype, 'leafStep', { get() { return 1 } })"_s, 1, [](const Guard& guard) {
        return guard.isOnMember(ClassSide::Prototype, "leafStep"_s) && guard.className == "Leaf"_s;
    });
}

static void checkRedefinedMember(LoadedProgram& loaded)
{
    RunningProgram program(loaded);
    program.evaluateModule(publishOpenClasses);
    program.checkClears("Object.defineProperty(Base.prototype, 'firstStep', { get() { return function () { } } })"_s, 3, [](const Guard& guard) {
        return guard.isOnMember(ClassSide::Prototype, "firstStep"_s);
    });
    program.checkClears("Object.defineProperty(Base.prototype, 'secondStep', { value: function () { } })"_s, 3, [](const Guard& guard) {
        return guard.isOnMember(ClassSide::Prototype, "secondStep"_s);
    });
}

static void checkShadowingNameOnSubclassPrototype(LoadedProgram& loaded)
{
    RunningProgram program(loaded);
    program.evaluateModule(publishOpenClasses);
    program.checkClears("Middle.prototype.firstStep = function () { }"_s, 2, [](const Guard& guard) {
        return guard.isOnMember(ClassSide::Prototype, "firstStep"_s) && guard.isOfOneOf({ "Middle"_s, "Leaf"_s });
    });
    program.checkClears("Leaf.prototype.secondStep = 1"_s, 1, [](const Guard& guard) {
        return guard.isOnMember(ClassSide::Prototype, "secondStep"_s) && guard.className == "Leaf"_s;
    });
    AOT_TEST_CHECK(program.isSet("Base"_s, ClassSide::Prototype, MemberGuardKind::Member, "firstStep"_s) && program.isSet("Middle"_s, ClassSide::Prototype, MemberGuardKind::Member, "secondStep"_s));
}

static void checkLinkChangedInTheMiddleOfTheChain(LoadedProgram& loaded)
{
    RunningProgram program(loaded);
    program.evaluateModule(publishOpenClasses);
    program.checkClears("Object.setPrototypeOf(Middle.prototype, { })"_s, 6, [](const Guard& guard) {
        if (guard.side != ClassSide::Prototype || guard.kind == MemberGuardKind::InstancePrototype || guard.kind == MemberGuardKind::NotHidden || !guard.isOfOneOf({ "Middle"_s, "Leaf"_s }))
            return false;
        // The engine reports the change of a prototype holder without a name, which stands for every member of the holder too.
        bool isDeclaredBelowTheHolder = guard.name == "leafStep"_s && guard.className == "Leaf"_s;
        return guard.kind == MemberGuardKind::PrototypeLink || !isDeclaredBelowTheHolder;
    });
    AOT_TEST_CHECK(program.isSet("Leaf"_s, ClassSide::Prototype, MemberGuardKind::Member, "leafStep"_s));
    AOT_TEST_CHECK(program.isSet("Base"_s, ClassSide::Prototype, MemberGuardKind::PrototypeLink) && program.isSet("Base"_s, ClassSide::Prototype, MemberGuardKind::Member, "firstStep"_s));
    AOT_TEST_CHECK(program.isSet("Middle"_s, ClassSide::Static, MemberGuardKind::PrototypeLink) && program.isSet("Leaf"_s, ClassSide::Static, MemberGuardKind::PrototypeLink));
}

static void checkLinkOfClassObjectChanged(LoadedProgram& loaded)
{
    RunningProgram program(loaded);
    program.evaluateModule(publishOpenClasses);
    program.checkClears("Object.setPrototypeOf(Middle, Function.prototype)"_s, 6, [](const Guard& guard) {
        return guard.side == ClassSide::Static && guard.isOfOneOf({ "Middle"_s, "Leaf"_s }) && guard.kind != MemberGuardKind::EvaluatedOnce;
    });
    AOT_TEST_CHECK(program.isSet("Base"_s, ClassSide::Static, MemberGuardKind::Member, "createOne"_s) && program.isSet("Leaf"_s, ClassSide::Prototype, MemberGuardKind::PrototypeLink));
}

static void checkUnguardedNameClearsNothing(LoadedProgram& loaded)
{
    RunningProgram program(loaded);
    program.evaluateModule(publishOpenClasses);
    for (ASCIILiteral source : { "Base.prototype.unusedStep = 1"_s, "delete Base.prototype.unusedStep"_s, "Base.prototype.brandNew = 1"_s, "Middle.prototype.brandNew = 1"_s, "Base.extra = 1"_s, "Leaf.extra = 1"_s, "delete Leaf.extra"_s, "new Leaf(1).brandNew = 1"_s, "Base.prototype[Symbol.iterator] = 1"_s, "use(1)"_s })
        program.checkClearsNothing(source);
}

static void checkOwnPropertyHidesMember(LoadedProgram& loaded)
{
    RunningProgram program(loaded);
    program.evaluateModule(publishOpenClasses);
    program.checkClears("new Leaf(1).firstStep = 1"_s, 1, [](const Guard& guard) {
        return guard.saysNoInstanceHides("firstStep"_s) && guard.className == "Leaf"_s;
    });
    program.checkClearsNothing("new Leaf(1).firstStep = 1"_s);
    program.checkClears("Object.defineProperty(new Middle(1), 'firstStep', { get() { return 1 } })"_s, 1, [](const Guard& guard) {
        return guard.saysNoInstanceHides("firstStep"_s) && guard.className == "Middle"_s;
    });
    AOT_TEST_CHECK(program.isSet("Base"_s, ClassSide::Prototype, MemberGuardKind::NotHidden, "firstStep"_s) && program.isSet("Leaf"_s, ClassSide::Prototype, MemberGuardKind::NotHidden, "secondStep"_s));
    AOT_TEST_CHECK(program.isSet("Leaf"_s, ClassSide::Prototype, MemberGuardKind::Member, "firstStep"_s) && program.isSet("Middle"_s, ClassSide::Prototype, MemberGuardKind::Member, "firstStep"_s));

    // Each object is tested for a name that no class declares.
    program.checkClearsNothing("new Leaf(1).absentStep = 1"_s);
    AOT_TEST_CHECK(program.isSet("Leaf"_s, ClassSide::Prototype, MemberGuardKind::Member, "absentStep"_s));
}

static bool saysWhatInstancesOfLeafFind(const Guard& guard)
{
    return guard.className == "Leaf"_s && guard.side == ClassSide::Prototype && (guard.kind == MemberGuardKind::NotHidden || guard.kind == MemberGuardKind::InstancePrototype);
}

static void checkInstanceGetsAnotherPrototype(LoadedProgram& loaded)
{
    RunningProgram program(loaded);
    program.evaluateModule(publishOpenClasses);
    program.checkClearsNothing("Object.setPrototypeOf(new Leaf(1), Leaf.prototype)"_s);
    program.checkClears("Object.setPrototypeOf(new Leaf(1), { })"_s, 4, saysWhatInstancesOfLeafFind);
    AOT_TEST_CHECK(program.isSet("Leaf"_s, ClassSide::Prototype, MemberGuardKind::PrototypeLink) && program.isSet("Leaf"_s, ClassSide::Static, MemberGuardKind::Member, "createOne"_s));
    AOT_TEST_CHECK(program.isSet("Middle"_s, ClassSide::Prototype, MemberGuardKind::NotHidden, "firstStep"_s) && program.isSet("Base"_s, ClassSide::Prototype, MemberGuardKind::NotHidden, "firstStep"_s));
    AOT_TEST_CHECK(program.isSet("Leaf"_s, ClassSide::Prototype, MemberGuardKind::Member, "firstStep"_s) && program.isSet("Leaf"_s, ClassSide::Prototype, MemberGuardKind::Member, "leafStep"_s));
}

static void checkInstancePrototypeLeadsBackToTheClass(LoadedProgram& loaded)
{
    RunningProgram program(loaded);
    program.evaluateModule(publishOpenClasses);
    program.checkClears("globalThis.between = Object.create(Leaf.prototype); Object.setPrototypeOf(new Leaf(1), between)"_s, 0, [](const Guard& guard) {
        return guard.className == "Leaf"_s && guard.kind == MemberGuardKind::InstancePrototype;
    });
    AOT_TEST_CHECK(program.isSet("Leaf"_s, ClassSide::Prototype, MemberGuardKind::NotHidden, "firstStep"_s) && program.isSet("Leaf"_s, ClassSide::Prototype, MemberGuardKind::NotHidden, "leafStep"_s));
    program.checkClearsNothing("between.brandNew = 1"_s);
    program.checkClears("between.firstStep = 1"_s, 1, [](const Guard& guard) {
        return guard.saysNoInstanceHides("firstStep"_s) && guard.className == "Leaf"_s;
    });

    program.checkClears("globalThis.hiding = Object.create(Leaf.prototype); hiding.leafStep = 1; Object.setPrototypeOf(new Leaf(1), hiding)"_s, 1, [](const Guard& guard) {
        return guard.saysNoInstanceHides("leafStep"_s) && guard.className == "Leaf"_s;
    });
    AOT_TEST_CHECK(program.isSet("Leaf"_s, ClassSide::Prototype, MemberGuardKind::NotHidden, "secondStep"_s));

    program.checkClears("Object.setPrototypeOf(between, { })"_s, 2, saysWhatInstancesOfLeafFind);
    AOT_TEST_CHECK(program.isSet("Middle"_s, ClassSide::Prototype, MemberGuardKind::NotHidden, "firstStep"_s) && program.isSet("Leaf"_s, ClassSide::Prototype, MemberGuardKind::PrototypeLink));
    AOT_TEST_CHECK(program.isSet("Leaf"_s, ClassSide::Prototype, MemberGuardKind::Member, "firstStep"_s) && program.isSet("Leaf"_s, ClassSide::Prototype, MemberGuardKind::Member, "leafStep"_s));
}

static void checkGuardIsNeverSetAgain(LoadedProgram& loaded)
{
    RunningProgram program(loaded);
    program.evaluateModule(publishOpenClasses);
    program.checkClears("globalThis.original = Base.prototype.firstStep; Base.prototype.firstStep = function () { }"_s, 3, [](const Guard& guard) {
        return guard.isOnMember(ClassSide::Prototype, "firstStep"_s);
    });
    for (ASCIILiteral source : { "Base.prototype.firstStep = original"_s, "Object.defineProperty(Base.prototype, 'firstStep', { value: original, writable: true, enumerable: false, configurable: true })"_s, "use(1)"_s, "defineLate()"_s }) {
        program.evaluate(source);
        for (ASCIILiteral className : { "Base"_s, "Middle"_s, "Leaf"_s })
            AOT_TEST_CHECK_FOR_SOURCE(!program.isSet(className, ClassSide::Prototype, MemberGuardKind::Member, "firstStep"_s), source);
    }

    program.evaluate("Object.setPrototypeOf(Middle.prototype, { }); Object.setPrototypeOf(Middle.prototype, Base.prototype)"_s);
    AOT_TEST_CHECK(!program.isSet("Middle"_s, ClassSide::Prototype, MemberGuardKind::PrototypeLink) && !program.isSet("Leaf"_s, ClassSide::Prototype, MemberGuardKind::PrototypeLink));
    program.checkNoSetOutlivesItsGuards();
}

static void checkStaticMember(LoadedProgram& loaded)
{
    RunningProgram program(loaded);
    program.evaluateModule(publishOpenClasses);
    program.checkClears("Leaf.createOne = function () { }"_s, 1, [](const Guard& guard) {
        return guard.isOnMember(ClassSide::Static, "createOne"_s) && guard.className == "Leaf"_s;
    });
    program.checkClears("Base.createOne = function () { }"_s, 1, [](const Guard& guard) {
        return guard.isOnMember(ClassSide::Static, "createOne"_s);
    });
}

static void checkStoreThatRunsStaticSetter(LoadedProgram& loaded)
{
    RunningProgram program(loaded);
    program.evaluateModule(publishOpenClasses);
    AOT_TEST_CHECK(program.isSet("Base"_s, ClassSide::Static, MemberGuardKind::Member, "level"_s) && program.isSet("Leaf"_s, ClassSide::Static, MemberGuardKind::Member, "level"_s));
    program.checkClearsNothing("Base.level = 5"_s);
    program.checkClearsNothing("Leaf.level = 5"_s);
    program.checkClearsNothing("Middle.level = 5"_s);
    AOT_TEST_CHECK(program.evaluate("!Object.hasOwn(Leaf, 'level') && !Object.hasOwn(Middle, 'level')"_s).isTrue());

    program.checkClears("Object.defineProperty(Leaf, 'level', { value: 1, writable: true })"_s, 1, [](const Guard& guard) {
        return guard.isOnMember(ClassSide::Static, "level"_s) && guard.className == "Leaf"_s;
    });
    program.checkClears("Object.defineProperty(Base, 'level', { value: 1, writable: true })"_s, 1, [](const Guard& guard) {
        return guard.isOnMember(ClassSide::Static, "level"_s);
    });
}

static void checkNameDeclaredByNoClass(LoadedProgram& loaded)
{
    RunningProgram program(loaded);
    program.evaluateModule(publishOpenClasses);
    AOT_TEST_CHECK(program.isSet("Base"_s, ClassSide::Prototype, MemberGuardKind::Member, "absentStep"_s) && program.isSet("Leaf"_s, ClassSide::Prototype, MemberGuardKind::Member, "absentStep"_s));
    program.checkClears("Middle.prototype.absentStep = 1"_s, 1, [](const Guard& guard) {
        return guard.isOnMember(ClassSide::Prototype, "absentStep"_s) && guard.isOfOneOf({ "Middle"_s, "Leaf"_s });
    });
    program.checkClears("Base.prototype.absentStep = 1"_s, 1, [](const Guard& guard) {
        return guard.isOnMember(ClassSide::Prototype, "absentStep"_s);
    });
}

static void checkNameAddedAboveTheClasses(LoadedProgram& loaded)
{
    RunningProgram program(loaded);
    program.evaluateModule(publishOpenClasses);
    AOT_TEST_CHECK(program.evaluate("use(1)[4] === undefined && use(1)[5] === undefined"_s).isTrue());
    bool isInherited = program.evaluate("try { Object.prototype.absentStep = 1; } catch { } 'absentStep' in Base.prototype"_s).isTrue();
    if (isInherited)
        AOT_TEST_CHECK(!program.isSet("Base"_s, ClassSide::Prototype, MemberGuardKind::Member, "absentStep"_s) && !program.isSet("Leaf"_s, ClassSide::Prototype, MemberGuardKind::Member, "absentStep"_s));
    AOT_TEST_CHECK(program.evaluate("use(1)[4] === new Base(1).absentStep && use(1)[5] === new Leaf(1).absentStep"_s).isTrue());
}

static void checkStaticField(LoadedProgram& loaded)
{
    RunningProgram program(loaded);
    program.evaluateModule(publishOpenClasses);
    program.evaluate("defineLate()"_s);
    program.checkClearsNothing("Base.tally = 5"_s);
    program.checkClearsNothing("Object.defineProperty(Base, 'tally', { value: 6 })"_s);
    program.checkClearsNothing("Object.keys(Base)"_s);
    program.checkClears("Leaf.tally = 7"_s, 1, [](const Guard& guard) {
        return guard.isOnMember(ClassSide::Static, "tally"_s) && guard.className == "Leaf"_s;
    });
    program.checkClears("Base.tally = 'text'"_s, 2, [](const Guard& guard) {
        return guard.isOnMember(ClassSide::Static, "tally"_s);
    });
}

static void checkSubclassDefinedAfterStaticFieldWasDeleted(LoadedProgram& loaded)
{
    RunningProgram program(loaded);
    program.evaluateModule(publishOpenClasses);
    program.checkClears("delete Base.tally"_s, 2, [](const Guard& guard) {
        return guard.isOnMember(ClassSide::Static, "tally"_s);
    });
    program.evaluate("defineLate()"_s);
    AOT_TEST_CHECK(!program.isSet("Late"_s, ClassSide::Static, MemberGuardKind::Member, "tally"_s));
    AOT_TEST_CHECK(program.isSet("Late"_s, ClassSide::Static, MemberGuardKind::Member, "createOne"_s) && program.isSet("Late"_s, ClassSide::Static, MemberGuardKind::PrototypeLink));
}

static void checkStaticFieldWhoseStringIsNotCreated(LoadedProgram& loaded)
{
    RunningProgram program(loaded);
    program.evaluateModule(publishOpenClasses);
    Closure& base = *uncheckedDowncast<Closure>(program.evaluate("Base"_s).asCell());
    const FieldData* mode = StaticFields::findOwnField(base, Identifier::fromString(program.vm(), "mode"_s).impl());
    AOT_TEST_CHECK(mode && mode->slot.representation() == SlotRepresentation::LiteralString && base.structure()->mayBePrototype());
    AOT_TEST_CHECK(!ProgramStatics::readVariableWithoutAllocating(program.state().instance, mode->slot));

    Guards before = program.readGuards();
    StaticFields::ensureProperties(program.vm(), base);
    unsigned attributes = 0;
    AOT_TEST_CHECK(isValidOffset(base.structure()->get(program.vm(), Identifier::fromString(program.vm(), "mode"_s), attributes)) && (attributes & PropertyAttribute::CustomValue));
    AOT_TEST_CHECK(program.isSet("Base"_s, ClassSide::Static, MemberGuardKind::Member, "mode"_s) && program.isSet("Leaf"_s, ClassSide::Static, MemberGuardKind::Member, "mode"_s));
    Guards after = program.readGuards();
    for (size_t index = 0; index < before.size(); ++index)
        AOT_TEST_CHECK(before[index].isSet == after[index].isSet);
    AOT_TEST_CHECK(program.evaluate("Base.mode === 'red' && Leaf.mode === 'red'"_s).isTrue());
}

static void checkSecondEvaluation(LoadedProgram& loaded)
{
    RunningProgram program(loaded);
    program.evaluateModule(publishOpenClasses);
    program.evaluate("globalThis.First = defineLate()[0]"_s);
    Guards before = program.readGuards();
    program.evaluate("globalThis.Second = defineLate()[0]"_s);
    Guards after = program.readGuards();
    unsigned numberOfGuardsOfClass = 0;
    for (size_t index = 0; index < before.size(); ++index) {
        bool isOfClass = before[index].className == "Late"_s;
        numberOfGuardsOfClass += isOfClass;
        AOT_TEST_CHECK(before[index].isSet && after[index].isSet == !isOfClass);
    }
    AOT_TEST_CHECK(numberOfGuardsOfClass >= 4 && valueOf(program.vm(), RuntimeCounterID::ClassesEvaluatedAgain) == 1);
    AOT_TEST_CHECK(program.evaluate("First !== Second && Object.getPrototypeOf(Second) === Base && new Second(1) instanceof Base && Second.tally === 0"_s).isTrue());

    program.checkClearsNothing("First.createOne = 1; Second.createOne = 1; Object.setPrototypeOf(First, Function.prototype)"_s);
}

static JSObject* prototypeOfClass(RunningProgram& program, ASCIILiteral className)
{
    JSCell* classObject = program.state().objects->closures.closureIfCreated(FunctionID(program.classNamed(className).classData().classConstructor));
    AOT_TEST_CHECK_FOR_SOURCE(classObject, className);
    return asObject(uncheckedDowncast<Closure>(classObject)->prototypeProperty(program.vm()));
}

static void checkPrototypeWithManyNamesKeepsItsGuards(LoadedProgram& loaded)
{
    RunningProgram program(loaded);
    program.evaluateModule(publishOpenClasses);
    program.checkClearsNothing("for (let i = 0; i < 600; i++) Base.prototype['added' + i] = i"_s);
    JSObject* prototype = prototypeOfClass(program, "Base"_s);
    AOT_TEST_CHECK(prototype->structure()->isDictionary() && !prototype->structure()->isUncacheableDictionary());

    prototype->flattenDictionaryObject(program.vm());
    AOT_TEST_CHECK(!prototype->structure()->isDictionary());
    for (const Guard& guard : program.readGuards())
        AOT_TEST_CHECK(guard.isSet);
    AOT_TEST_CHECK(!valueOf(program.vm(), RuntimeCounterID::MemberGuardsCleared) && !valueOf(program.vm(), RuntimeCounterID::PrototypeLinkGuardsCleared));

    program.checkClears("Base.prototype.firstStep = function () { }"_s, 3, [](const Guard& guard) {
        return guard.isOnMember(ClassSide::Prototype, "firstStep"_s);
    });
    program.checkClears("delete Base.prototype.secondStep"_s, 3, [](const Guard& guard) {
        return guard.isOnMember(ClassSide::Prototype, "secondStep"_s);
    });
}

static void checkLazyPrototypeKeepsItsBits(LoadedProgram& loaded)
{
    RunningProgram program(loaded);
    AOT_TEST_CHECK(program.classNamed("Lazy"_s).createsMembersLazily && program.classNamed("Shadowing"_s).classData().guardsHoldWhileClear);
    AOT_TEST_CHECK(program.isSet("Shadowing"_s, ClassSide::Prototype, MemberGuardKind::Member, "lazyStep"_s));
    program.evaluateModule(publishLazyClass);
    AOT_TEST_CHECK(program.isSet("Shadowing"_s, ClassSide::Prototype, MemberGuardKind::Member, "lazyStep"_s));

    program.checkClearsNothing("useShadowing()"_s);
    JSObject* lazyPrototype = asObject(prototypeOfClass(program, "Shadowing"_s)->getPrototypeDirect());
    AOT_TEST_CHECK(lazyPrototype->classInfo() == ClassPrototype::info());

    PropertyNameArrayBuilder names(program.vm(), PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
    lazyPrototype->methodTable()->getOwnPropertyNames(lazyPrototype, program.realm(), names, DontEnumPropertiesMode::Include);
    Identifier lazyStep = Identifier::fromString(program.vm(), "lazyStep"_s);
    AOT_TEST_CHECK(lazyPrototype->classInfo() != ClassPrototype::info() && lazyPrototype->getDirect(program.vm(), lazyStep));
    AOT_TEST_CHECK(program.isSet("Shadowing"_s, ClassSide::Prototype, MemberGuardKind::Member, "lazyStep"_s) && !valueOf(program.vm(), RuntimeCounterID::MemberGuardsCleared));

    PutPropertySlot slot(lazyPrototype);
    AOT_TEST_CHECK(lazyPrototype->methodTable()->put(lazyPrototype, program.realm(), lazyStep, jsNumber(1), slot));
    unsigned numberOfGuardsOnMember = 0;
    for (const Guard& guard : program.readGuards()) {
        if (!guard.isOnMember(ClassSide::Prototype, "lazyStep"_s))
            continue;
        ++numberOfGuardsOnMember;
        AOT_TEST_CHECK(!guard.isSet && guard.isOfOneOf({ "Lazy"_s, "Shadowing"_s }));
    }
    AOT_TEST_CHECK(numberOfGuardsOnMember && !program.isSet("Shadowing"_s, ClassSide::Prototype, MemberGuardKind::Member, "lazyStep"_s));
    AOT_TEST_CHECK(valueOf(program.vm(), RuntimeCounterID::MemberGuardsCleared) == numberOfGuardsOnMember);
}

// The guards are armed when the class is created, and then nobody knows what the member under the computed name will hide. Once it is defined, what the class inherits is looked up.
static void checkInheritedNameBelowComputedMember(LoadedProgram& loaded)
{
    RunningProgram program(loaded);
    AOT_TEST_CHECK(!program.classNamed("Computed"_s).classData().guardsHoldWhileClear);
    program.evaluateModule("globalThis.nameOfMember = Symbol('member'); globalThis.publish = function (useComputed) { globalThis.useComputed = useComputed; };"_s);
    AOT_TEST_CHECK(program.isSet("Computed"_s, ClassSide::Prototype, MemberGuardKind::Member, "step"_s));
    program.checkClearsNothing("useComputed()"_s);
}

// The name that it turns out to be hides what the class inherits under it.
static void checkComputedMemberHidesInheritedName(LoadedProgram& loaded)
{
    RunningProgram program(loaded);
    program.evaluateModule("globalThis.nameOfMember = 'step'; globalThis.publish = function (useComputed) { globalThis.useComputed = useComputed; };"_s);
    AOT_TEST_CHECK(!program.isSet("Computed"_s, ClassSide::Prototype, MemberGuardKind::Member, "step"_s));
}

static void checkNameAddedBelowLazyPrototype(LoadedProgram& loaded)
{
    RunningProgram program(loaded);
    program.evaluateModule(publishLazyClass);
    program.checkClears("shadow('lazyStep', function () { })"_s, 1, [](const Guard& guard) {
        return guard.isOnMember(ClassSide::Prototype, "lazyStep"_s) && guard.className == "Shadowing"_s;
    });
    program.checkClearsNothing("useShadowing()"_s);
}

namespace {

struct SetsOverBothClasses {
    uint32_t membersThatNoInstanceHides;
};

} // anonymous namespace

static SetsOverBothClasses findSetsOverBothClasses(RunningProgram& program)
{
    Vector<uint32_t> setsOfLate = program.setsWith("Late"_s, MemberGuardKind::Member, "step"_s);
    Vector<uint32_t> setsOfInstances = program.setsWith("Late"_s, MemberGuardKind::NotHidden, "step"_s);
    Vector<uint32_t> sets = program.setsWith("Early"_s, MemberGuardKind::Member, "step"_s);
    sets.removeAllMatching([&](uint32_t set) {
        return !setsOfLate.contains(set);
    });
    AOT_TEST_CHECK(setsOfInstances.size() == 1 && sets.contains(setsOfInstances[0]));
    AOT_TEST_CHECK(program.classNamed("Early"_s).classData().guardsHoldWhileClear && program.classNamed("Late"_s).classData().guardsHoldWhileClear);
    AOT_TEST_CHECK(setsOfInstances[0] & MemberGuardAggregates::holdsWhileClear);
    return { setsOfInstances[0] };
}

static void checkSetOverClassDefinedLater(LoadedProgram& loaded)
{
    RunningProgram program(loaded);
    SetsOverBothClasses sets = findSetsOverBothClasses(program);
    AOT_TEST_CHECK(program.isSetNumbered(sets.membersThatNoInstanceHides));

    program.evaluateModule(publishClassDefinedLater);
    AOT_TEST_CHECK(program.isSet("Early"_s, ClassSide::Prototype, MemberGuardKind::Member, "step"_s) && program.isSet("Late"_s, ClassSide::Prototype, MemberGuardKind::Member, "step"_s));
    AOT_TEST_CHECK(program.isSetNumbered(sets.membersThatNoInstanceHides));
    AOT_TEST_CHECK(program.evaluate("callStep(new Early)"_s) == jsNumber(7));

    program.evaluate("globalThis.late = defineLate()"_s);
    AOT_TEST_CHECK(program.isSet("Late"_s, ClassSide::Prototype, MemberGuardKind::Member, "step"_s) && program.isSet("Late"_s, ClassSide::Prototype, MemberGuardKind::NotHidden, "step"_s));
    AOT_TEST_CHECK(program.isSetNumbered(sets.membersThatNoInstanceHides));
    AOT_TEST_CHECK(program.evaluate("callStep(late)"_s) == jsNumber(7));

    program.evaluate("late.step = function () { return 9 }"_s);
    AOT_TEST_CHECK(!program.isSetNumbered(sets.membersThatNoInstanceHides));
    AOT_TEST_CHECK(program.evaluate("callStep(late)"_s) == jsNumber(9) && program.evaluate("callStep(new Early)"_s) == jsNumber(7));
    program.checkNoSetOutlivesItsGuards();
}

static void checkClassDefinedLaterFindsChangedMember(LoadedProgram& loaded)
{
    RunningProgram program(loaded);
    SetsOverBothClasses sets = findSetsOverBothClasses(program);
    program.evaluateModule(publishClassDefinedLater);
    AOT_TEST_CHECK(program.isSetNumbered(sets.membersThatNoInstanceHides));

    program.evaluate("Early.prototype.step = function () { return 8 }"_s);
    AOT_TEST_CHECK(!program.isSetNumbered(sets.membersThatNoInstanceHides));
    AOT_TEST_CHECK(program.evaluate("callStep(new Early)"_s) == jsNumber(8));

    program.evaluate("globalThis.late = defineLate()"_s);
    AOT_TEST_CHECK(!program.isSet("Late"_s, ClassSide::Prototype, MemberGuardKind::Member, "step"_s));
    AOT_TEST_CHECK(!program.isSetNumbered(sets.membersThatNoInstanceHides));
    AOT_TEST_CHECK(program.evaluate("callStep(late)"_s) == jsNumber(8));
    program.checkNoSetOutlivesItsGuards();
}

static void checkClassObjectNobodyReads(LoadedProgram& loaded)
{
    RunningProgram program(loaded);
    program.evaluateModule(publishClassObjectNobodyReads);
    program.evaluate("globalThis.quiet = make()"_s);
    AOT_TEST_CHECK(program.isSet("Quiet"_s, ClassSide::Prototype, MemberGuardKind::Member, "step"_s) && program.isSet("Quiet"_s, ClassSide::Prototype, MemberGuardKind::NotHidden, "step"_s));
    AOT_TEST_CHECK(program.evaluate("callStep(quiet)"_s) == jsNumber(7));

    program.evaluate("Object.getPrototypeOf(quiet).step = function () { return 8 }"_s);
    AOT_TEST_CHECK(!program.isSet("Quiet"_s, ClassSide::Prototype, MemberGuardKind::Member, "step"_s));
    AOT_TEST_CHECK(program.evaluate("callStep(quiet)"_s) == jsNumber(8));
    program.checkNoSetOutlivesItsGuards();
}

template<const ASCIILiteral& programText, LoadedProgramTest body>
static void runNativeTestWithCompiledCode()
{
    for (const NativeTest& test : memberGuardTests()) {
        if (test.run != &runNativeTestWithCompiledCode<programText, body>)
            continue;
        if (const char* imagePath = byteCast<char>(Options::aotImagePath()))
            useImageWithCodeMappedFromFile(imagePath);
        // Hot, so that the calls rest on guards: cold code, and much of these programs runs once, keeps its calls by name and asks for no guard.
        runWithLoadedProgram(test.name, programText, body, { "heat=EverythingIsHot"_s });
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

template<LoadedProgramTest body>
static constexpr void (*withOpenClasses)() = runNativeTestWithCompiledCode<programWithOpenClassesText, body>;

template<LoadedProgramTest body>
static constexpr void (*withLazyClass)() = runNativeTestWithCompiledCode<programWithLazyClassText, body>;

template<LoadedProgramTest body>
static constexpr void (*withComputedMember)() = runNativeTestWithCompiledCode<programWithComputedMemberText, body>;

template<LoadedProgramTest body>
static constexpr void (*withClassDefinedLater)() = runNativeTestWithCompiledCode<programWithClassDefinedLaterText, body>;

template<LoadedProgramTest body>
static constexpr void (*withClassObjectNobodyReads)() = runNativeTestWithCompiledCode<programWithClassObjectNobodyReadsText, body>;

std::span<const NativeTest> memberGuardTests()
{
    static constexpr NativeTest tests[] = {
        { "member-guards/set-at-the-definition"_s, withOpenClasses<checkGuardsAreSetAtTheDefinition> },
        { "member-guards/changed-member-of-base-class"_s, withOpenClasses<checkChangedMemberOfBaseClass> },
        { "member-guards/redefined-member"_s, withOpenClasses<checkRedefinedMember> },
        { "member-guards/shadowing-name-on-subclass-prototype"_s, withOpenClasses<checkShadowingNameOnSubclassPrototype> },
        { "member-guards/link-changed-in-the-middle-of-the-chain"_s, withOpenClasses<checkLinkChangedInTheMiddleOfTheChain> },
        { "member-guards/link-of-class-object-changed"_s, withOpenClasses<checkLinkOfClassObjectChanged> },
        { "member-guards/unguarded-name-clears-nothing"_s, withOpenClasses<checkUnguardedNameClearsNothing> },
        { "member-guards/own-property-hides-member"_s, withOpenClasses<checkOwnPropertyHidesMember> },
        { "member-guards/prototype-with-many-names-keeps-its-guards"_s, withOpenClasses<checkPrototypeWithManyNamesKeepsItsGuards> },
        { "member-guards/instance-gets-another-prototype"_s, withOpenClasses<checkInstanceGetsAnotherPrototype> },
        { "member-guards/instance-prototype-leads-back-to-the-class"_s, withOpenClasses<checkInstancePrototypeLeadsBackToTheClass> },
        { "member-guards/never-set-again"_s, withOpenClasses<checkGuardIsNeverSetAgain> },
        { "member-guards/static-member"_s, withOpenClasses<checkStaticMember> },
        { "member-guards/store-running-a-static-setter"_s, withOpenClasses<checkStoreThatRunsStaticSetter> },
        { "member-guards/name-declared-by-no-class"_s, withOpenClasses<checkNameDeclaredByNoClass> },
        { "member-guards/name-added-above-the-classes"_s, withOpenClasses<checkNameAddedAboveTheClasses> },
        { "member-guards/stores-to-static-field"_s, withOpenClasses<checkStaticField> },
        { "member-guards/subclass-defined-after-static-field-was-deleted"_s, withOpenClasses<checkSubclassDefinedAfterStaticFieldWasDeleted> },
        { "member-guards/static-field-whose-string-is-not-created"_s, withOpenClasses<checkStaticFieldWhoseStringIsNotCreated> },
        { "member-guards/second-evaluation"_s, withOpenClasses<checkSecondEvaluation> },
        { "member-guards/lazy-prototype-keeps-its-bits"_s, withLazyClass<checkLazyPrototypeKeepsItsBits> },
        { "member-guards/name-added-below-lazy-prototype"_s, withLazyClass<checkNameAddedBelowLazyPrototype> },
        { "member-guards/inherited-name-below-a-computed-member"_s, withComputedMember<checkInheritedNameBelowComputedMember> },
        { "member-guards/computed-member-hides-an-inherited-name"_s, withComputedMember<checkComputedMemberHidesInheritedName> },
        { "member-guards/set-over-a-class-defined-later"_s, withClassDefinedLater<checkSetOverClassDefinedLater> },
        { "member-guards/class-defined-later-finds-a-changed-member"_s, withClassDefinedLater<checkClassDefinedLaterFindsChangedMember> },
        { "member-guards/class-object-nobody-reads"_s, withClassObjectNobodyReads<checkClassObjectNobodyReads> },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
