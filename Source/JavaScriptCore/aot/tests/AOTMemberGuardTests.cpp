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
#include "AOTModuleHost.h"
#include "AOTObjectModel.h"
#include "AOTProgramIdentifiers.h"
#include "AOTProgramStatics.h"
#include "AOTRuntimeCounters.h"
#include "AOTStaticFields.h"
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
layout L1 kind=Instance name="Middle" module=m0 predictedBase=L0 constructor=f2 supertypes=[L0] flags=[HasHeritage]
layout L2 kind=Instance name="Leaf" module=m0 predictedBase=L1 constructor=f3 supertypes=[L0, L1] flags=[HasHeritage]
layout L3 kind=Instance name="Late" module=m0 predictedBase=L0 constructor=f14 supertypes=[L0] flags=[HasHeritage]
layout L4 kind=Instance name="Foreign" module=m0 constructor=f16 flags=[HasHeritage]
variable g0 name="tally" module=m0 declared={type=number} owner=L0
variable g1 name="mode" module=m0 declared={type="red"|"green"} owner=L0
variable g2 name="Base" module=m0 declared={type=anything} initialization=Undefined
variable g3 name="Middle" module=m0 declared={type=anything} initialization=Undefined
variable g4 name="Leaf" module=m0 declared={type=anything} initialization=Undefined
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
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
  b0(v0: anything, v1: number):
    v2 = GetConstructPrototype v0 layout=L0
    v3 = NewObject v2 layout=L0
    PropSet v3, v1 key="value"
    Return v3
}
function f2 name="Middle" kind=DerivedConstructor flags=[HasNewTarget, TakesCallee] parameters=[{type=number}] results=[{type=anything}] {
  b0(v0: function, v1: anything, v2: number):
    v3 = GetSuperBase v0
    v4 = SuperConstruct v3, v1, v2 layout=L1
    Return v4
}
function f3 name="Leaf" kind=DerivedConstructor flags=[HasNewTarget, TakesCallee] parameters=[{type=number}] results=[{type=anything}] {
  b0(v0: function, v1: anything, v2: number):
    v3 = GetSuperBase v0
    v4 = SuperConstruct v3, v1, v2 layout=L2
    Return v4
}
function f4 name="firstStep" kind=Method flags=[HasThis] results=[{type=anything}] {
  b0(v0: anything):
    Return
}
function f5 name="secondStep" kind=Method flags=[HasThis] results=[{type=anything}] {
  b0(v0: anything):
    Return
}
function f6 name="unusedStep" kind=Method flags=[HasThis] results=[{type=anything}] {
  b0(v0: anything):
    Return
}
function f7 name="createOne" kind=Method flags=[HasThis] results=[{type=anything}] {
  b0(v0: anything):
    Return
}
function f8 name="initializer" kind=Initializer flags=[HasThis] thisType={type=function} results=[{type=anything}] {
  b0(v0: function):
    v1 = Integer integer=0
    PropDefine v0, v1 key="tally"
    v3 = String string="red"
    PropDefine v0, v3 key="mode"
    Return
}
function f9 name="middleStep" kind=Method flags=[HasThis] results=[{type=anything}] {
  b0(v0: anything):
    Return
}
function f10 name="leafStep" kind=Method flags=[HasThis] results=[{type=anything}] {
  b0(v0: anything):
    Return
}
function f11 name="use" parameters=[{type=number}] results=[{type=anything}] {
  b0(v0: number):
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
    v31 = NewArray v23, v24, v25, v26, v29, v30 contents=anything holes=[]
    Return v31
}
function f12 name="mutate" parameters=[{type=anything}, {type=string}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: string, v2: anything):
    ElemSet v0, v1, v2
    v4 = ElemDelete v0, v1
    v5 = Builtin builtin=ObjectSetPrototypeOf
    v6 = Call v5, -, v0, v2
    Return
}
function f13 name="level" kind=Setter flags=[HasThis] parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything):
    Return
}
function f14 name="Late" kind=DerivedConstructor flags=[HasNewTarget, TakesCallee] parameters=[{type=number}] results=[{type=anything}] {
  b0(v0: function, v1: anything, v2: number):
    v3 = GetSuperBase v0
    v4 = SuperConstruct v3, v1, v2 layout=L3
    Return v4
}
function f15 name="defineLate" results=[{type=anything}] {
  b0():
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
  b0(v0: function, v1: anything):
    v2 = GetSuperBase v0
    v3 = SuperConstruct v2, v1 layout=L4
    Return v3
}
function f17 name="defineForeign" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
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
  b0(v0: anything):
    Return
}
)IR"_s;

static constexpr ASCIILiteral programWithLazyClassText = R"IR(module m0 path="/main.js" displayPath="main.js" body=f0
entry module=m0
layout L0 kind=Instance name="Lazy" module=m0 constructor=f1
layout L1 kind=Instance name="Shadowing" module=m0 predictedBase=L0 constructor=f2 supertypes=[L0] flags=[HasHeritage]
variable g0 name="Shadowing" module=m0 declared={type=anything} initialization=Undefined
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
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
  b0(v0: anything):
    v1 = GetConstructPrototype v0 layout=L0
    v2 = NewObject v1 layout=L0
    Return v2
}
function f2 name="Shadowing" kind=DerivedConstructor flags=[HasNewTarget, TakesCallee] results=[{type=anything}] {
  b0(v0: function, v1: anything):
    v2 = GetSuperBase v0
    v3 = SuperConstruct v2, v1 layout=L1
    Return v3
}
function f3 name="lazyStep" kind=Method flags=[HasThis] results=[{type=anything}] {
  b0(v0: anything):
    Return
}
function f4 name="useShadowing" results=[{type=anything}] {
  b0():
    v0 = GetModuleVariable variable=g0
    v1 = Construct v0
    v2 = PropGet v1 key="lazyStep"
    v3 = Call v2, v1 calleeKey="lazyStep"
    Return
}
function f5 name="shadow" parameters=[{type=string}, {type=anything}] results=[{type=anything}] {
  b0(v0: string, v1: anything):
    v2 = GetModuleVariable variable=g0
    v3 = PropGet v2 key="prototype"
    ElemSet v3, v0, v1
    Return
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
    AOT_TEST_CHECK(!scope.exception() && isEvaluated(realm(), *module));
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
            String name = guard.kind() == MemberGuardKind::Member ? ProgramIdentifiers::textOf(*Image::current(), guard.name) : String();
            bool isSet = state().objects->classes.guardWords(layout, guard.side())[guard.wordIndex()] & guard.mask();
            guards.append({ className, guard.side(), guard.kind(), name, isSet });
        }
    });
    return guards;
}

bool RunningProgram::isSet(ASCIILiteral className, ClassSide side, MemberGuardKind kind, ASCIILiteral name)
{
    for (const Guard& guard : readGuards()) {
        if (guard.className == className && guard.isOn(side, kind) && (kind != MemberGuardKind::Member || guard.name == name))
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

    unsigned numberOfMembers = 0;
    unsigned numberOfLinks = 0;
    for (size_t index = 0; index < before.size(); ++index) {
        bool isCleared = before[index].isSet && isExpectedToBeCleared(before[index]);
        numberOfMembers += isCleared && before[index].kind == MemberGuardKind::Member;
        numberOfLinks += isCleared && before[index].kind == MemberGuardKind::PrototypeLink;
        if (after[index].isSet != (before[index].isSet && !isCleared)) {
            dataLogLn(before[index].className, " side ", static_cast<unsigned>(before[index].side), " kind ", static_cast<unsigned>(before[index].kind), " ", before[index].name, " is ", after[index].isSet ? "set" : "clear");
            failNativeTest(__FILE__, __LINE__, source.characters());
        }
    }
    AOT_TEST_CHECK_FOR_SOURCE(numberOfMembers + numberOfLinks >= minimumNumber, source);
    AOT_TEST_CHECK_FOR_SOURCE(valueOf(vm(), RuntimeCounterID::MemberGuardsCleared) == membersBefore + numberOfMembers, source);
    AOT_TEST_CHECK_FOR_SOURCE(valueOf(vm(), RuntimeCounterID::PrototypeLinkGuardsCleared) == linksBefore + numberOfLinks, source);
}

void RunningProgram::checkClearsNothing(ASCIILiteral source)
{
    checkClears(source, 0, [](const Guard&) {
        return false;
    });
}

static constexpr ASCIILiteral publishOpenClasses = "globalThis.publish = function (Base, Middle, Leaf, use, mutate, defineLate, defineForeign) { globalThis.Base = Base; globalThis.Middle = Middle; globalThis.Leaf = Leaf; globalThis.use = use; globalThis.defineLate = defineLate; globalThis.defineForeign = defineForeign }"_s;
static constexpr ASCIILiteral publishLazyClass = "globalThis.publish = function (useShadowing, shadow) { globalThis.useShadowing = useShadowing; globalThis.shadow = shadow }"_s;

static void checkGuardsAreSetAtTheDefinition(LoadedProgram& loaded)
{
    RunningProgram program(loaded);
    AOT_TEST_CHECK(program.readGuards().size() >= 20);
    for (const Guard& guard : program.readGuards())
        AOT_TEST_CHECK(!guard.isSet);

    program.evaluateModule(publishOpenClasses);
    for (const Guard& guard : program.readGuards())
        AOT_TEST_CHECK(guard.isSet == guard.isOfOneOf({ "Base"_s, "Middle"_s, "Leaf"_s }));
    AOT_TEST_CHECK(program.isSet("Base"_s, ClassSide::Prototype, MemberGuardKind::Member, "firstStep"_s) && program.isSet("Leaf"_s, ClassSide::Prototype, MemberGuardKind::Member, "firstStep"_s));
    AOT_TEST_CHECK(program.isSet("Leaf"_s, ClassSide::Static, MemberGuardKind::Member, "createOne"_s) && program.isSet("Leaf"_s, ClassSide::Static, MemberGuardKind::Member, "tally"_s));
    AOT_TEST_CHECK(program.isSet("Middle"_s, ClassSide::Prototype, MemberGuardKind::PrototypeLink) && program.isSet("Middle"_s, ClassSide::Static, MemberGuardKind::PrototypeLink));

    program.evaluate("defineLate()"_s);
    for (const Guard& guard : program.readGuards())
        AOT_TEST_CHECK(guard.isSet == !guard.isOfOneOf({ "Foreign"_s }));
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
        if (guard.side != ClassSide::Prototype || !guard.isOfOneOf({ "Middle"_s, "Leaf"_s }))
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
    for (ASCIILiteral source : { "Base.prototype.unusedStep = 1"_s, "delete Base.prototype.unusedStep"_s, "Base.prototype.brandNew = 1"_s, "Middle.prototype.brandNew = 1"_s, "Base.extra = 1"_s, "Leaf.extra = 1"_s, "delete Leaf.extra"_s, "new Leaf(1).firstStep = 1"_s, "Base.prototype[Symbol.iterator] = 1"_s, "use(1)"_s })
        program.checkClearsNothing(source);
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
    bool isInherited = program.evaluate("Object.prototype.absentStep = 1; 'absentStep' in Base.prototype"_s).isTrue();
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
        AOT_TEST_CHECK(before[index].isSet == !before[index].isOfOneOf({ "Foreign"_s }) && after[index].isSet == (before[index].isSet && !isOfClass));
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

static void checkLazyPrototypeKeepsItsBits(LoadedProgram& loaded)
{
    RunningProgram program(loaded);
    AOT_TEST_CHECK(program.classNamed("Lazy"_s).createsMembersLazily && !program.isSet("Shadowing"_s, ClassSide::Prototype, MemberGuardKind::Member, "lazyStep"_s));
    program.evaluateModule(publishLazyClass);
    AOT_TEST_CHECK(program.isSet("Shadowing"_s, ClassSide::Prototype, MemberGuardKind::Member, "lazyStep"_s));

    JSObject* lazyPrototype = asObject(prototypeOfClass(program, "Shadowing"_s)->getPrototypeDirect());
    AOT_TEST_CHECK(lazyPrototype->classInfo() == ClassPrototype::info());
    program.checkClearsNothing("useShadowing()"_s);
    AOT_TEST_CHECK(lazyPrototype->classInfo() == ClassPrototype::info());

    PropertyNameArrayBuilder names(program.vm(), PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
    lazyPrototype->methodTable()->getOwnPropertyNames(lazyPrototype, program.realm(), names, DontEnumPropertiesMode::Include);
    Identifier lazyStep = Identifier::fromString(program.vm(), "lazyStep"_s);
    AOT_TEST_CHECK(lazyPrototype->classInfo() != ClassPrototype::info() && lazyPrototype->getDirect(program.vm(), lazyStep));
    AOT_TEST_CHECK(program.isSet("Shadowing"_s, ClassSide::Prototype, MemberGuardKind::Member, "lazyStep"_s) && !valueOf(program.vm(), RuntimeCounterID::MemberGuardsCleared));

    PutPropertySlot slot(lazyPrototype);
    AOT_TEST_CHECK(lazyPrototype->methodTable()->put(lazyPrototype, program.realm(), lazyStep, jsNumber(1), slot));
    AOT_TEST_CHECK(!program.isSet("Shadowing"_s, ClassSide::Prototype, MemberGuardKind::Member, "lazyStep"_s) && valueOf(program.vm(), RuntimeCounterID::MemberGuardsCleared) == 1);
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

template<const ASCIILiteral& programText, LoadedProgramTest body>
static void runNativeTestWithCompiledCode()
{
    for (const NativeTest& test : memberGuardTests()) {
        if (test.run != &runNativeTestWithCompiledCode<programText, body>)
            continue;
        if (const char* imagePath = byteCast<char>(Options::aotImagePath()))
            useImageWithCodeMappedFromFile(imagePath);
        runWithLoadedProgram(test.name, programText, body);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

template<LoadedProgramTest body>
static constexpr void (*withOpenClasses)() = runNativeTestWithCompiledCode<programWithOpenClassesText, body>;

template<LoadedProgramTest body>
static constexpr void (*withLazyClass)() = runNativeTestWithCompiledCode<programWithLazyClassText, body>;

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
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
