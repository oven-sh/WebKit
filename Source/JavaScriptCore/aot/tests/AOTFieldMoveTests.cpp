/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTAssignmentPlans.h"
#include "AOTBuildContext.h"
#include "AOTChildProcess.h"
#include "AOTCollectionTestSupport.h"
#include "AOTCompile.h"
#include "AOTCompileOptionWords.h"
#include "AOTFieldMoves.h"
#include "AOTFunctionNames.h"
#include "AOTFunctionTables.h"
#include "AOTIRParser.h"
#include "AOTImage.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTObjectModelCaches.h"
#include "AOTProgramFile.h"
#include "AOTRuntimeCounters.h"
#include "AOTTemporaryFile.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include "JSCInlines.h"
#include "ObjectConstructor.h"
#include "Options.h"
#include "StrongInlines.h"
#include <cmath>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <wtf/DataLog.h>
#include <wtf/text/MakeString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

std::span<const NativeTest> fieldMoveTests();

static constexpr ASCIILiteral programText = R"IR(layout L0 name="Typed" fields=[{key="u8" declared={type=int[0, 255]} flags=[IsRequired]}, {key="i8" declared={type=int[-128, 127]} flags=[IsRequired]}, {key="u16" declared={type=int[0, 65535]} flags=[IsRequired]}, {key="i16" declared={type=int[-32768, 32767]} flags=[IsRequired]}, {key="i32" declared={type=i32} flags=[IsRequired]}, {key="u32" declared={type=int[0, 4294967295]} flags=[IsRequired]}, {key="i64" declared={type=int} flags=[IsRequired]}, {key="oi64" declared={type=undefined|null|int} flags=[IsRequired]}, {key="num" declared={type=number} flags=[IsRequired]}, {key="onum" declared={type=undefined|null|number} flags=[IsRequired]}, {key="bool" declared={type=boolean} flags=[IsRequired]}, {key="obool" declared={type=undefined|null|boolean} flags=[IsRequired]}, {key="str" declared={type=string} flags=[IsRequired]}, {key="ostr" declared={type=undefined|null|string} flags=[IsRequired]}, {key="any" declared={type=anything} flags=[IsRequired]}, {key="lit" declared={type="red"|"green"} flags=[IsRequired]}, {key="olit" declared={type=undefined|null|"red"|"green"} flags=[IsRequired]}, {key="fn" declared={type=f3} flags=[IsRequired]}, {key="oi32" declared={type=undefined|null|int[-2147483643, 2147483647]} flags=[IsRequired]}]
module m0 path="/main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="publish" [isFreeName]
    v2 = NewClosure function=f1
    v3 = NewClosure function=f2
    v4 = NewClosure function=f3
    v5 = NewClosure function=f4
    v6 = Call v1, -, v2, v3, v4, v5
    Return
}
function f1 name="make" parameters=[{type=int[0, 255]}, {type=int[-128, 127]}, {type=int[0, 65535]}, {type=int[-32768, 32767]}, {type=i32}, {type=int[0, 4294967295]}, {type=int}, {type=undefined|null|int}, {type=number}, {type=undefined|null|number}, {type=boolean}, {type=undefined|null|boolean}, {type=string}, {type=undefined|null|string}, {type=anything}, {type="red"|"green"}, {type=undefined|null|"red"|"green"}, {type=f3}, {type=undefined|null|int[-2147483643, 2147483647]}] results=[{type=anything}] {
    v19 = NewObject -, v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18 layout=L0 fields=[0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18]
    v20 = FinishObject v19
    Return v20
}
function f2 name="mutate" parameters=[{type=anything}, {type=string}, {type=anything}] results=[{type=anything}] {
    ElemSet v0, v1, v2
    v4 = ElemDelete v0, v1
    Return
}
function f3 name="callback" results=[{type=anything}] {
    Return
}
function f4 name="otherCallback" results=[{type=anything}] {
    Return
}
)IR"_s;

static Vector<uint8_t> compileWithNarrowReferences()
{
    Vector<uint8_t> image;
    runOnBuildThread([&] {
        Wire::CompileOptions options = makeDefaultCompileOptions();
        options.executableIdentity = { };
        options.target.referenceWidth = Wire::ReferenceWidth::Narrow;
        BuildContext build(options);
        std::unique_ptr<Typed::Program> program = Typed::parseProgram(Typed::withStandInLibrary(String(programText)), Typed::IRForm::AsRead, build);
        AOT_TEST_CHECK(program);
        std::unique_ptr<CompileOutput> output = compileParsedProgram(WTF::move(program), build);
        if (output->image.isEmpty())
            Typed::printDiagnostics(output->diagnostics, WTF::dataFile());
        AOT_TEST_CHECK(!output->image.isEmpty());
        image = WTF::move(output->image);
    });
    return image;
}

static void runWithNarrowReferences(ASCIILiteral testName, LoadedProgramTest body)
{
    if (Options::aotImagePath()) {
        runWithLoadedProgram(testName, programText, body);
        return;
    }
    TemporaryFile file(".image"_s, compileWithNarrowReferences().span());
    ChildProcessLauncher launcher(String(programPath()), { });
    Vector<String> arguments { String(nativeSubcommandName), String(singleProcessFlag), String(testName), makeString("--aotImagePath="_s, file.path()) };
    ChildProcessResult result = launcher.run(arguments.span());
    if (!result.exitedWith(0))
        dataLogLn(result.output);
    AOT_TEST_CHECK(result.exitedWith(0));
}

template<LoadedProgramTest body>
static void runNativeTestWithNarrowReferences()
{
    for (const NativeTest& test : fieldMoveTests()) {
        if (test.run == &runNativeTestWithNarrowReferences<body>) {
            runWithNarrowReferences(test.name, body);
            return;
        }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static const LayoutData& typedLayout(LoadedProgram& program)
{
    const ProgramTables& tables = program.state().tables;
    for (uint32_t shape = 1; shape < tables.firstEnvironmentShape; ++shape) {
        if (tables.hasLayoutForShape(shape) && Image::current()->text().equals(StringID(tables.layoutForShape(shape).name), "Typed"_s))
            return tables.layoutForShape(shape);
    }
    failNativeTest(__FILE__, __LINE__, "the image has the layout");
}

static RuntimeLayoutTable& layoutsOf(LoadedProgram& program)
{
    return program.state().objects->layouts;
}

static const FieldData& typedField(LoadedProgram& program, ASCIILiteral name)
{
    const FieldData* field = layoutsOf(program).field(program.vm(), typedLayout(program), Identifier::fromString(program.vm(), name).impl());
    AOT_TEST_CHECK(field);
    return *field;
}

static FunctionID functionNamed(LoadedProgram& program, ASCIILiteral name)
{
    const ProgramTables& tables = programTables();
    const uint64_t* closureWords = FunctionTables::of(program.vm()).words;
    for (uint32_t index = tables.closureFunctions.first; index < tables.closureFunctions.end; ++index) {
        if (closureWords[index] && writtenFunctionName(FunctionID(index)) == name)
            return FunctionID(index);
    }
    failNativeTest(__FILE__, __LINE__, name.characters());
}

static constexpr ASCIILiteral nameOfFieldWithTypeFor(SlotRepresentation representation)
{
    switch (representation) {
    case SlotRepresentation::Uint8:
        return "u8"_s;
    case SlotRepresentation::Int8:
        return "i8"_s;
    case SlotRepresentation::Uint16:
        return "u16"_s;
    case SlotRepresentation::Int16:
        return "i16"_s;
    case SlotRepresentation::Int32:
        return "i32"_s;
    case SlotRepresentation::Uint32:
        return "u32"_s;
    case SlotRepresentation::Int64:
        return "i64"_s;
    case SlotRepresentation::OptionalInt64:
        return "oi64"_s;
    case SlotRepresentation::OptionalInt32:
        return "oi32"_s;
    case SlotRepresentation::Float:
    case SlotRepresentation::Double:
        return "num"_s;
    case SlotRepresentation::OptionalDouble:
        return "onum"_s;
    case SlotRepresentation::Boolean:
        return "bool"_s;
    case SlotRepresentation::OptionalBoolean:
        return "obool"_s;
    case SlotRepresentation::Cell:
    case SlotRepresentation::Cell32:
        return "str"_s;
    case SlotRepresentation::OptionalCell32:
        return "ostr"_s;
    case SlotRepresentation::JSValue:
        return "any"_s;
    case SlotRepresentation::LiteralString:
    case SlotRepresentation::LiteralString32:
        return "olit"_s;
    case SlotRepresentation::DeferredClosure:
        return "fn"_s;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

namespace {

struct LayoutWithEveryRepresentation {
    static constexpr unsigned numberOfFields = numberOfEnumerators<SlotRepresentation>;

    explicit LayoutWithEveryRepresentation(LoadedProgram&);

    LayoutData layout { };
    FieldData fields[numberOfFields] { };
    uint32_t operands[1] { };
    uint32_t references[numberOfInlineReferenceCounts + numberOfFields] { };
};
static_assert(offsetof(LayoutWithEveryRepresentation, fields) == offsetof(LayoutWithEveryRepresentation, layout) + sizeof(LayoutData));
static_assert(offsetof(LayoutWithEveryRepresentation, operands) == offsetof(LayoutWithEveryRepresentation, fields) + sizeof(LayoutWithEveryRepresentation::fields));

class Fixture {
    WTF_MAKE_NONCOPYABLE(Fixture);
public:
    explicit Fixture(LoadedProgram&);

    VM& vm() { return m_program.vm(); }
    RuntimeLayoutTable& layouts() { return layoutsOf(m_program); }
    const LayoutData& layout() const { return m_layout.layout; }
    std::span<const FieldData> fields() const { return m_layout.layout.fields(); }
    const FieldData& field(SlotRepresentation representation) const { return fields()[static_cast<unsigned>(representation)]; }
    Structure* structure() { return m_structure.get(); }
    const MarkedArgumentBuffer& samples() const { return m_samples; }

    Object* create() { return Object::create(vm(), structure(), layout(), layout().size, unorderedOrderWord); }
    Object* createHolding(const FieldData&, JSValue);
    bool isSame(JSValue a, JSValue b) { return a && b && sameValue(m_program.realm(), a, b); }
    bool holdsInitialBits(Object* object, const FieldData& field) { return !memcmp(object->slotOf(field), create()->slotOf(field), widthOf(field.slot.representation())); }
    using FieldPair = std::pair<const FieldData*, const FieldData*>;
    bool tryCopyByMap(Object* target, Object* source, std::initializer_list<FieldPair>);

private:
    LoadedProgram& m_program;
    LayoutWithEveryRepresentation& m_layout;
    Strong<Structure> m_structure;
    MarkedArgumentBuffer m_samples;
};

} // anonymous namespace

LayoutWithEveryRepresentation::LayoutWithEveryRepresentation(LoadedProgram& program)
{
    Vector<uint32_t, numberOfFields> wideReferences;
    Vector<uint32_t, numberOfFields> narrowReferences;
    uint32_t offset = offsetOfFirstField;
    for (SlotRepresentation representation : allMembersOf<SlotRepresentation>) {
        const FieldData& model = typedField(program, nameOfFieldWithTypeFor(representation));
        SlotTraits traits = traitsOf(representation);
        offset = roundUpToMultipleOf(traits.width, offset);
        FieldData& field = fields[static_cast<unsigned>(representation)];
        field.name = model.name;
        field.type = model.type;
        field.slot = SlotData::create(offset, representation);
        if (traits.holdsCell)
            (traits.isNarrowReference ? narrowReferences : wideReferences).append(offset);
        offset += traits.width;
    }
    operands[0] = functionNamed(program, "callback"_s).index();

    uint32_t* next = references;
    *next++ = wideReferences.size();
    *next++ = 0;
    *next++ = narrowReferences.size();
    for (uint32_t reference : wideReferences)
        *next++ = reference;
    for (uint32_t reference : narrowReferences)
        *next++ = reference;

    const LayoutData& model = typedLayout(program);
    layout.kindBits = static_cast<uint32_t>(LayoutKind::Record);
    layout.keysAreNeverEnumerated = true;
    layout.index = model.index;
    layout.shape = model.shape;
    layout.name = model.name;
    layout.size = roundUpToMultipleOf<16>(offset);
    layout.numberOfFields = numberOfFields;
    layout.numberOfFieldOperands = std::size(operands);
    layout.referencesOffset = offsetof(LayoutWithEveryRepresentation, references) - offsetof(LayoutWithEveryRepresentation, layout);
}

Fixture::Fixture(LoadedProgram& program)
    : m_program(program)
    , m_layout(*new LayoutWithEveryRepresentation(program))
{
    VM& vm = program.vm();
    AOT_TEST_CHECK(!Image::current()->header().options.useWideReferences());
    m_structure.set(vm, Object::createStructure(vm, { .realm = program.realm(), .prototype = program.realm()->objectPrototype(), .layout = &m_layout.layout }));

    for (double number : { 0.0, 1.0, -1.0, 127.0, 128.0, -128.0, -129.0, 255.0, 256.0, 32767.0, 32768.0, -32768.0, -32769.0, 65535.0, 65536.0, 2147483647.0, 2147483648.0, -2147483648.0, -2147483645.0, -2147483649.0, 4294967295.0, 4294967296.0, 1099511627776.0, 9007199254740991.0, -9007199254740991.0, -0.0, 0.5, 1.5, 0.1, 1e300, std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN() })
        m_samples.append(jsNumber(number));
    for (JSValue value : { jsBoolean(true), jsBoolean(false), jsUndefined(), jsNull() })
        m_samples.append(value);
    for (ASCIILiteral text : { "red"_s, "green"_s, "blue"_s, ""_s })
        m_samples.append(jsString(vm, String(text)));
    m_samples.append(constructEmptyObject(program.realm()));
    m_samples.append(program.state().objects->closures.closure(vm, functionNamed(program, "callback"_s)));
    m_samples.append(program.state().objects->closures.closure(vm, functionNamed(program, "otherCallback"_s)));
}

Object* Fixture::createHolding(const FieldData& field, JSValue value)
{
    Object* object = create();
    std::optional<AcceptedValue> accepted = layouts().accept(field, value);
    if (!accepted || !object->store(vm(), field, *accepted))
        return nullptr;
    AOT_TEST_CHECK(isSame(object->read(field), value));
    return object;
}

// A copy that leaves out the names of a run goes by the map between objects of one layout too. What the map knows of such a copy is written here as tryDescribeMappedCopy() writes it, with a move for each pair of fields.
bool Fixture::tryCopyByMap(Object* target, Object* source, std::initializer_list<FieldPair> pairs)
{
    constexpr uint32_t excludedRunNumberPlusOne = 1;
    State& state = m_program.state();
    MappedCopy& copy = state.objects->caches->assignmentPlans.mappedCopyAt(target->orderAndShapeWords(), source->orderAndShapeWords(), excludedRunNumberPlusOne);
    copy = { };
    copy.targetWords = target->orderAndShapeWords();
    copy.sourceWords = source->orderAndShapeWords();
    copy.excludedRunNumberPlusOne = excludedRunNumberPlusOne;
    copy.orderWordAfter = target->orderWord();
    copy.state = MappedCopy::State::IsServed;
    for (auto [from, to] : pairs) {
        SlotRepresentation representationOfSource = from->slot.representation();
        SlotRepresentation representationOfTarget = to->slot.representation();
        bool isTested = !alwaysFits(layouts(), *from, *to);
        uint8_t acceptedKinds = isTested ? kindsAlwaysAccepted(layouts().typeOf(*to)).toRaw() : 0;
        copy.moves.append(MappedCopy::Move { from->slot.offset, to->slot.offset, moveKindFor(representationOfSource, representationOfTarget, isTested), static_cast<uint8_t>(widthOf(representationOfTarget)),
            false, false, representationOfSource, representationOfTarget, acceptedKinds, layout().indexOf(*to) });
        copy.mayStoreCells |= traitsOf(representationOfTarget).holdsCell;
    }
    return tryRepeatAssignment(vm(), state, target, JSValue(source), excludedRunNumberPlusOne) == RepeatedCopy::ByMap;
}

// AssignmentPlanCache::describeMappedField() leaves a closure that may not be made yet to a plan.
static bool isMovedByMap(const FieldData& field)
{
    return field.slot.representation() != SlotRepresentation::DeferredClosure;
}

static void checkEveryPairOfRepresentations(LoadedProgram& program)
{
    Fixture fixture(program);
    unsigned numberOfPairsWithMove = 0;
    unsigned numberOfPairsWithRefusal = 0;
    for (const FieldData& from : fixture.fields()) {
        for (const FieldData& to : fixture.fields()) {
            if (!isMovedByMap(from) || !isMovedByMap(to))
                continue;
            bool hasMove = false;
            bool hasRefusal = false;
            for (unsigned index = 0; index < fixture.samples().size(); ++index) {
                Object* source = fixture.createHolding(from, fixture.samples().at(index));
                if (!source)
                    continue;
                JSValue value = source->read(from);
                bool fits = !!fixture.createHolding(to, value);
                Object* target = fixture.create();
                AOT_TEST_CHECK(fixture.tryCopyByMap(target, source, { { &from, &to } }) == fits);
                AOT_TEST_CHECK(fits ? fixture.isSame(target->read(to), value) : fixture.holdsInitialBits(target, to));
                AOT_TEST_CHECK(fixture.isSame(source->read(from), value));
                (fits ? hasMove : hasRefusal) = true;
            }
            AOT_TEST_CHECK(hasMove || hasRefusal);
            AOT_TEST_CHECK(&from != &to || (hasMove && !hasRefusal));
            numberOfPairsWithMove += hasMove;
            numberOfPairsWithRefusal += hasRefusal;
        }
    }
    AOT_TEST_CHECK(numberOfPairsWithMove > fixture.fields().size() && numberOfPairsWithRefusal > fixture.fields().size());
}

static void checkAbsentSourceIsNeverMoved(LoadedProgram& program)
{
    Fixture fixture(program);
    unsigned numberOfSources = 0;
    for (const FieldData& from : fixture.fields()) {
        if (!from.canLeaveSlot() || !isMovedByMap(from))
            continue;
        ++numberOfSources;
        for (const FieldData& to : fixture.fields()) {
            if (!isMovedByMap(to))
                continue;
            Object* source = fixture.create();
            source->clear(from);
            Object* target = fixture.create();
            AOT_TEST_CHECK(!source->isInSlot(from) && !source->read(from));
            AOT_TEST_CHECK(!fixture.tryCopyByMap(target, source, { { &from, &to } }) && fixture.holdsInitialBits(target, to));
        }
    }
    AOT_TEST_CHECK(numberOfSources >= 10);
}

static void checkNothingIsStoredWhenOneMoveFails(LoadedProgram& program)
{
    Fixture fixture(program);
    const FieldData& number = fixture.field(SlotRepresentation::Double);
    const FieldData& optionalNumber = fixture.field(SlotRepresentation::OptionalDouble);
    const FieldData& anything = fixture.field(SlotRepresentation::JSValue);
    const FieldData& text = fixture.field(SlotRepresentation::Cell);

    Object* source = fixture.createHolding(number, jsNumber(1.5));
    AOT_TEST_CHECK(source->tryStore(fixture.vm(), anything, jsNumber(7)));
    Object* target = fixture.create();
    AOT_TEST_CHECK(!fixture.tryCopyByMap(target, source, { { &number, &optionalNumber }, { &anything, &text } }));
    AOT_TEST_CHECK(!target->isInSlot(optionalNumber) && !target->isInSlot(text));

    AOT_TEST_CHECK(source->tryStore(fixture.vm(), anything, jsNontrivialString(fixture.vm(), "text"_s)));
    AOT_TEST_CHECK(fixture.tryCopyByMap(target, source, { { &number, &optionalNumber }, { &anything, &text } }));
    AOT_TEST_CHECK(target->read(optionalNumber) == jsNumber(1.5) && target->read(text) == source->read(anything));
}

static void checkEveryRepresentationToPropertyOfNewObject(LoadedProgram& program)
{
    Fixture fixture(program);
    VM& vm = fixture.vm();
    PropertyOffset offset = invalidOffset;
    Structure* empty = JSFinalObject::createStructure(vm, program.realm(), program.realm()->objectPrototype(), 2);
    Strong<Structure> structure(vm, Structure::addPropertyTransition(vm, empty, Identifier::fromString(vm, "copied"_s), 0, offset));
    AOT_TEST_CHECK(isInlineOffset(offset));
    for (const FieldData& from : fixture.fields()) {
        FieldMovePlan plan;
        plan.appendField(from, offset);
        plan.structure = structure.get();
        AOT_TEST_CHECK(plan.moves.size() + plan.boxedWordMoves.size() == 1);
        unsigned numberOfValues = 0;
        for (unsigned index = 0; index < fixture.samples().size(); ++index) {
            Object* source = fixture.createHolding(from, fixture.samples().at(index));
            if (!source)
                continue;
            ++numberOfValues;
            JSFinalObject* copy = JSFinalObject::create(vm, structure.get());
            AOT_TEST_CHECK(applyMovesToNewObject(vm, plan, copy, source) && fixture.isSame(copy->getDirect(offset), source->read(from)));
        }
        AOT_TEST_CHECK(numberOfValues);
        if (!from.canLeaveSlot())
            continue;
        Object* withoutValue = fixture.create();
        withoutValue->clear(from);
        AOT_TEST_CHECK(!withoutValue->read(from) && !applyMovesToNewObject(vm, plan, JSFinalObject::create(vm, structure.get()), withoutValue));
    }
}

static void checkAnswersAboutTypesHoldForEveryValue(LoadedProgram& program)
{
    Fixture fixture(program);
    unsigned numberOfFittingPairs = 0;
    unsigned numberOfOtherPairs = 0;
    for (const FieldData& from : fixture.fields()) {
        OptionSet<ValueKind> accepted = kindsAlwaysAccepted(fixture.layouts().typeOf(from));
        for (unsigned index = 0; index < fixture.samples().size(); ++index) {
            JSValue value = fixture.samples().at(index);
            if (accepted.containsAny(valueKindOf(value)))
                AOT_TEST_CHECK(fixture.layouts().fieldAccepts(from, value));
        }
        for (const FieldData& to : fixture.fields()) {
            bool fits = alwaysFits(fixture.layouts(), from, to);
            ++(fits ? numberOfFittingPairs : numberOfOtherPairs);
            for (unsigned index = 0; fits && index < fixture.samples().size(); ++index) {
                JSValue value = fixture.samples().at(index);
                AOT_TEST_CHECK(!fixture.layouts().slotAdmits(from, value) || fixture.layouts().slotAdmits(to, value));
            }
        }
    }
    AOT_TEST_CHECK(numberOfFittingPairs > fixture.fields().size() && numberOfOtherPairs > fixture.fields().size());

    AOT_TEST_CHECK(alwaysFits(fixture.layouts(), fixture.field(SlotRepresentation::Uint8), fixture.field(SlotRepresentation::Int16)));
    AOT_TEST_CHECK(!alwaysFits(fixture.layouts(), fixture.field(SlotRepresentation::Int16), fixture.field(SlotRepresentation::Uint8)));
    AOT_TEST_CHECK(alwaysFits(fixture.layouts(), fixture.field(SlotRepresentation::Double), fixture.field(SlotRepresentation::OptionalDouble)));
    AOT_TEST_CHECK(!alwaysFits(fixture.layouts(), fixture.field(SlotRepresentation::OptionalDouble), fixture.field(SlotRepresentation::Double)));
    // A field that takes anything takes what a field of objects holds. The other way round it does not fit.
    AOT_TEST_CHECK(!fixture.layouts().typeOf(typedField(program, "fn"_s)).cellTests().empty());
    AOT_TEST_CHECK(alwaysFits(fixture.layouts(), typedField(program, "fn"_s), typedField(program, "any"_s)));
    AOT_TEST_CHECK(!alwaysFits(fixture.layouts(), typedField(program, "any"_s), typedField(program, "fn"_s)));
    // The type of a field that is declared to hold int32s leaves out the five words that its slot needs for markers.
    AOT_TEST_CHECK(kindsAlwaysAccepted(fixture.layouts().typeOf(fixture.field(SlotRepresentation::Int32))).isEmpty());
    AOT_TEST_CHECK(kindsAlwaysAccepted(fixture.layouts().typeOf(fixture.field(SlotRepresentation::Int64))) == OptionSet<ValueKind> { ValueKind::Int32 });
    AOT_TEST_CHECK(kindsAlwaysAccepted(fixture.layouts().typeOf(fixture.field(SlotRepresentation::Uint8))).isEmpty());
    AOT_TEST_CHECK(kindsAlwaysAccepted(fixture.layouts().typeOf(fixture.field(SlotRepresentation::OptionalBoolean))) == (OptionSet<ValueKind> { ValueKind::True, ValueKind::False, ValueKind::Undefined, ValueKind::Null }));
    AOT_TEST_CHECK(valueKindOf(jsNumber(1)) == ValueKind::Int32 && valueKindOf(jsNumber(1.5)) == ValueKind::Double && valueKindOf(constructEmptyObject(program.realm())).isEmpty());
}

static FieldMovePlanCache& planCacheOf(LoadedProgram& program)
{
    return program.state().objects->caches->movePlans;
}

static FieldMovePlanKey keyWithWords(uint64_t sourceWords, Structure* sourceStructure = nullptr)
{
    FieldMovePlanKey key;
    key.sourceWords = sourceWords;
    key.targetWords = 1;
    key.sourceStructure = sourceStructure;
    return key;
}

static void checkPlanCacheNeverRefuses(LoadedProgram& program)
{
    constexpr unsigned numberOfKeys = 5000;
    constexpr unsigned capacity = 256 * 4;
    FieldMovePlanCache& cache = planCacheOf(program);
    AOT_TEST_CHECK(!cache.find(keyWithWords(0)));

    for (unsigned index = 0; index < numberOfKeys; ++index) {
        auto plan = makeUnique<FieldMovePlan>();
        plan->moves.append({ .from = index });
        const FieldMovePlan* added = &cache.add(program.vm(), keyWithWords(index), WTF::move(plan));
        AOT_TEST_CHECK(cache.find(keyWithWords(index)) == added && !added->isUsable());
    }

    unsigned numberFound = 0;
    for (unsigned index = 0; index < numberOfKeys; ++index) {
        if (const FieldMovePlan* plan = cache.find(keyWithWords(index))) {
            AOT_TEST_CHECK(plan->moves[0].from == index);
            ++numberFound;
        }
    }
    AOT_TEST_CHECK(numberFound && numberFound <= capacity && cache.find(keyWithWords(numberOfKeys - 1)) && !cache.find(keyWithWords(numberOfKeys)));

    FieldMovePlanKey otherTarget = keyWithWords(numberOfKeys - 1);
    otherTarget.targetWords = 2;
    AOT_TEST_CHECK(!cache.find(otherTarget));
}

static constexpr unsigned numberOfShortLivedStructures = 16;

static NEVER_INLINE Vector<Structure*> addPlansOfShortLivedStructures(LoadedProgram& program, bool isSource)
{
    Vector<Structure*> structures;
    for (unsigned index = 0; index < numberOfShortLivedStructures; ++index) {
        Structure* structure = JSFinalObject::createStructure(program.vm(), program.realm(), jsNull(), 0);
        auto plan = makeUnique<FieldMovePlan>();
        if (!isSource)
            plan->structure = structure;
        planCacheOf(program).add(program.vm(), keyWithWords(index, isSource ? structure : nullptr), WTF::move(plan));
        structures.append(structure);
    }
    return structures;
}

static void checkPlansDieWithTheirStructures(LoadedProgram& program)
{
    VM& vm = program.vm();
    FieldMovePlanCache& cache = planCacheOf(program);
    for (bool isSource : { true, false }) {
        Strong<Structure> longLived(vm, JSFinalObject::createStructure(vm, program.realm(), jsNull(), 0));
        auto planOfLongLived = makeUnique<FieldMovePlan>();
        planOfLongLived->structure = longLived.get();
        cache.add(vm, keyWithWords(1000, longLived.get()), WTF::move(planOfLongLived));
        cache.add(vm, keyWithWords(1001), makeUnique<FieldMovePlan>());
        Vector<Structure*> structures = addPlansOfShortLivedStructures(program, isSource);

        vm.heap.collectSync(CollectionScope::Full);
        unsigned numberOfDeadStructures = 0;
        for (unsigned index = 0; index < structures.size(); ++index) {
            bool isLive = hasSurvivedCollection(vm, structures[index]);
            numberOfDeadStructures += !isLive;
            AOT_TEST_CHECK(!!cache.find(keyWithWords(index, isSource ? structures[index] : nullptr)) == isLive);
        }
        AOT_TEST_CHECK(numberOfDeadStructures);
        AOT_TEST_CHECK(cache.find(keyWithWords(1000, longLived.get())) && cache.find(keyWithWords(1001)));
    }
}

static void checkAssignmentTestsEachClosure(LoadedProgram& program)
{
    VM& vm = program.vm();
    const ProgramTables& tables = program.state().tables;
    const LayoutData& layout = typedLayout(program);
    const FieldData& field = typedField(program, "fn"_s);
    std::span<const RuntimeCellTest> tests = layoutsOf(program).typeOf(field).cellTests();
    AOT_TEST_CHECK(tests.size() == 1 && tests[0].kind == CellTestKind::IsClosureOf && tests[0].function() == functionNamed(program, "callback"_s));

    JSCell* accepted = program.state().objects->closures.closure(vm, functionNamed(program, "callback"_s));
    JSCell* refused = program.state().objects->closures.closure(vm, functionNamed(program, "otherCallback"_s));
    AOT_TEST_CHECK(accepted != refused && accepted->structure() == refused->structure());

    Identifier name = Identifier::fromString(vm, "fn"_s);
    JSObject* sourceOfAccepted = constructEmptyObject(program.realm());
    sourceOfAccepted->putDirect(vm, name, accepted);
    JSObject* sourceOfRefused = constructEmptyObject(program.realm());
    sourceOfRefused->putDirect(vm, name, refused);
    AOT_TEST_CHECK(sourceOfAccepted->structure() == sourceOfRefused->structure());

    Object* target = nullptr;
    for (uint32_t number = 0; !target && number < tables.allocations().size(); ++number) {
        if (tables.allocations()[number].shape == layout.shape)
            target = uncheckedDowncast<Object>(program.state().objects->structures.allocate(vm, number));
    }
    std::optional<AcceptedValue> acceptedByField = layoutsOf(program).accept(field, accepted);
    AOT_TEST_CHECK(target && acceptedByField && target->store(vm, field, *acceptedByField));

    uint64_t exitsBefore = valueOf(vm, RuntimeCounterID::FieldsLeftSlotByStoreOfOtherType);
    AOT_TEST_CHECK(PropertyWalk::tryAssign(vm, target, sourceOfAccepted));
    AOT_TEST_CHECK(target->isInSlot(field) && target->read(field) == JSValue(accepted));
    AOT_TEST_CHECK(valueOf(vm, RuntimeCounterID::FieldsLeftSlotByStoreOfOtherType) == exitsBefore);

    if (!PropertyWalk::tryAssign(vm, target, sourceOfRefused)) {
        AOT_TEST_CHECK(target->read(field) == JSValue(accepted));
        PutPropertySlot slot(target);
        AOT_TEST_CHECK(target->methodTable()->put(target, program.realm(), name, refused, slot));
    }
    AOT_TEST_CHECK(isCompiledObject(target) && !target->isInSlot(field) && !target->read(field));
    AOT_TEST_CHECK(target->get(program.realm(), name) == JSValue(refused));
    AOT_TEST_CHECK(valueOf(vm, RuntimeCounterID::FieldsLeftSlotByStoreOfOtherType) == exitsBefore + 1);
}

std::span<const NativeTest> fieldMoveTests()
{
    static constexpr NativeTest tests[] = {
        { "field-moves/every-pair-of-representations"_s, runNativeTestWithNarrowReferences<checkEveryPairOfRepresentations> },
        { "field-moves/absent-source-is-never-moved"_s, runNativeTestWithNarrowReferences<checkAbsentSourceIsNeverMoved> },
        { "field-moves/nothing-is-stored-when-one-move-fails"_s, runNativeTestWithNarrowReferences<checkNothingIsStoredWhenOneMoveFails> },
        { "field-moves/every-representation-to-a-property-of-a-new-object"_s, runNativeTestWithNarrowReferences<checkEveryRepresentationToPropertyOfNewObject> },
        { "field-moves/answers-about-types-hold-for-every-value"_s, runNativeTestWithNarrowReferences<checkAnswersAboutTypesHoldForEveryValue> },
        { "field-moves/plan-cache-never-refuses"_s, runNativeTestWithNarrowReferences<checkPlanCacheNeverRefuses> },
        { "field-moves/plans-die-with-their-structures"_s, runNativeTestWithNarrowReferences<checkPlansDieWithTheirStructures> },
        { "field-moves/assignment-tests-each-closure"_s, runNativeTestWithNarrowReferences<checkAssignmentTestsEachClosure> },
    };
    return tests;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
