/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFacts.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuiltinTable.h"
#include "AOTBuiltinTableRealm.h"
#include "AOTIRProgram.h"
#include "AOTRegExpFlags.h"
#include "AOTRegExpRecord.h"
#include "AOTRuntimeHooks.h"
#include "AOTSort.h"
#include "AOTThinSummary.h"
#include "Collector.h"
#include "ErrorInstance.h"
#include "IteratorOperations.h"
#include "JSCInlines.h"
#include "JSLock.h"
#include "JSNativeStdFunction.h"
#include "JSPromise.h"
#include "MarkedVector.h"
#include "ObjectConstructor.h"
#include "RegExpObject.h"
#include "TopExceptionScope.h"
#include "YarrFlags.h"
#include "YarrPattern.h"
#include <wtf/BitVector.h>
#include <wtf/HashSet.h>
#include <wtf/SetForScope.h>
#include <wtf/text/StringBuilder.h>

namespace JSC { namespace AOT { namespace Typed {

// WHICH TEXTS ARRIVE AT RegExp. A text that is listed gets a record and code in the image; one that is not is parsed, and matched by a flat program, when the program runs. So the list claims nothing, and nothing here is proven.
// The code that leads to a call of RegExp is REHEARSED in a realm of the build. First the body of the module that the program starts with,
// as the program goes through it: what it imports and requires, the functions that it calls, the classes and constructor functions that it constructs. `app.get("/users/:id", ..)`: the text comes to RegExp many calls down from there.
// An async function is gone through in one go: what is awaited is the value that a settled promise holds, or what nobody knows, and the call gives a promise that is settled. Many a server registers its routes behind an `await`.
// Then each function that the start of the program has handed to code that nobody knows (`Bun.serve({ fetch })`), once, with nothing known of what it is handed. Then each function that calls RegExp, with nothing known of what it is handed, and whoever names such a function.
// What the rehearsal does not know is one object of the realm, which throws where something asks for its text. Where a branch hangs on it both sides are gone through, up to the block where they meet again. From there the way goes on as one:
// a parameter of that block holds what the ways that know it bring it, or what nobody knows where they differ. Outside loops it goes on once for each of the values instead, a few times in a function. So a branch that nobody knows ends no loop around it.
// WHAT A PROGRAM DOES WITH THE PATTERNS THAT IT HAS MADE IS NOT REHEARSED: a built-in that is handed one answers what nobody knows, and costs nothing. A pattern that is written is matched: a text is often taken apart and escaped by such.
// THE LIST FOLLOWS FROM THE PROGRAM ALONE. The realm is the engine's own, with nothing of the host: no file, no process, no environment. No built-in whose row reads or writes ExternalState is called (the clock, chance,
// the time zone, the locale of the host, other agents), nor one that evaluates text, loads a module or starts an instance. One thread goes through the starts in the order of their numbers,
// and the engine starts none beside it: no machine code is made of bytecode, and that thread collects alone.

// WHAT BOUNDS IT. What lies behind a bound is compiled when the program runs.
// Code that throws: the operation gives what nobody knows, and a way that ends in a throw ends. Code that loops or calls without end: the allowances of operations. Code that allocates without end: the last three.
// THE ALLOWANCES GUARD AGAINST ONE THING: code is run here, and nobody can tell of code whether it ends. They count what is gone through for nothing: behind each text that it finds a start has its allowance anew, and what led there is given back.
// A function that is handed nothing that anybody knows is not entered: it does what it does from a start of its own. One that calls itself behind a branch that nobody knows is not entered again: nobody knows where that ends, and each call may make two more.
// EVERY VALUE LIES WHERE THE COLLECTOR SEES IT (the stack, a MarkedVector, an object of the realm), so the realm is collected like any other, under whatever options the process has.
static constexpr uint32_t operationsFromOneStart = 1 << 17;
// A start is the body of a module that the program starts with, a function that it hands to code that nobody knows, a function of the program's own that calls RegExp with a text that is not written, or one that names such a function.
// The first two kinds in all, and the last two in all, go through no more than this many times the operations that the bodies of the program hold,
// and what a few starts that never end use up. A program that does little else than make patterns needs twice what it holds, a large command-line application a quarter.
static constexpr uint64_t operationsInAllForEachOfProgram = 4;
// A text that is found costs this much, and nothing else: `for (let i = 0; ; i++) new RegExp("a" + i)` ends too.
static constexpr uint64_t operationsForText = 1 << 8;
static constexpr uint64_t operationsInAllOfSmallestProgram = 8 * operationsFromOneStart;
// A router lays a path into a tree of its characters, one call a character. A call takes 3 KB of the stack, of which the engine allows 5 MB; where a built-in finds no room it throws, and that ends the rehearsal.
// The bound is a number, and not the room that is left, so that the list is the same on every host.
static constexpr unsigned deepestCall = 256;
// To enter a function costs as many operations as it has values, divided by this: room is made for each value.
static constexpr uint32_t valuesMadeRoomForAtPriceOfOneOperation = 16;
static constexpr uint8_t timesThroughBlockWhileGoingBothWays = 4;
// No text is longer than longestRegExpPatternWithCode, and no built-in is handed a greater number, which may set how much it allocates.
// A built-in costs as many operations as the texts and lists that it is handed have characters and elements: a sort, a join, a search.
// One that is handed a RegExp may match, and a match may go back over its subject again and again: it costs this much, and an operation for each character. A pattern with a repeat inside a repeat is not matched at all.
static constexpr uint32_t operationsForBuiltinHandedRegExp = 1 << 10;
// With this many bytes in the realm, or that many characters made in all, the rehearsal is over, with what it has found. So it is where the engine reports that a limit of its own is reached:
// no memory, no stack, a termination.
static constexpr size_t mostBytesOfRealm = 256 * MB;

// `(a+)+$`: a match goes back over its subject in as many ways as there are to deal it out among the turns, and gives up at Yarr::matchLimit: half a second.
static bool repeatsInsideRepeat(const Yarr::PatternDisjunction& disjunction, bool isInsideRepeat)
{
    for (const auto& alternative : disjunction.m_alternatives) {
        for (const Yarr::PatternTerm& term : alternative->m_terms) {
            bool varies = term.quantityType != Yarr::QuantifierType::FixedCount;
            if (varies && isInsideRepeat)
                return true;
            bool hasParentheses = term.type == Yarr::PatternTerm::Type::ParenthesesSubpattern || term.type == Yarr::PatternTerm::Type::ParentheticalAssertion;
            if (hasParentheses && repeatsInsideRepeat(*term.parentheses.disjunction, isInsideRepeat || term.quantityMaxCount > 1u))
                return true;
        }
    }
    return false;
}

class RehearsalOfRegExpTexts {
    WTF_MAKE_NONCOPYABLE(RehearsalOfRegExpTexts);
public:
    // Whoever makes it holds the lock of the VM. It lies on the stack: MarkedVector.
    RehearsalOfRegExpTexts(VM&, const Program&, const BodyIndexTable&, const ThinSummaries&);

    void run(std::span<const FunctionID> functionsThatCallRegExp);
    void storeTexts(ProgramFactTables&);

private:
    enum class StateOfModule : uint8_t { IsNotEntered, IsRunning, HasRun };

    using Values = MarkedVector<JSValue, 8, RecordOverflow>;

    // From a branch on a value that nobody knows to the block where its sides meet again. That block is not entered from inside: what the edges bring it is kept until both sides are through.
    struct OpenRegion {
        BlockID join;
        // In Activation::arrivals.
        size_t firstArrival { 0 };
        uint32_t numberOfArrivals { 0 };
        bool hasBroughtOtherValues { false };
    };

    struct Activation {
        const BodyIndex& index;
        FunctionID function;
        Values values;
        // While a region is open. By block.
        Vector<uint8_t> timesEnteredWhileGoingBothWays;
        Vector<BlockID, 8> blocksCounted;
        Vector<OpenRegion, 2> regions;
        // For each edge that has come to the join of an open region, what it brings the parameters of that block, one behind the other. No two of one region bring the same.
        Values arrivals;
        const Vector<BlockID>* joins { nullptr };
        // How often the code behind a join was gone through once more, for other values.
        unsigned numberOfWaysGoneSeparately { 0 };
        bool waysDifferInWhatTheyReturn { false };

        bool goesBothWays() const { return !regions.isEmpty(); }
    };

    struct ClosureOfProgram {
        FunctionID function;
        JSArray* captures { nullptr };
    };

    bool isKnown(JSValue value) const { return value != JSValue(m_unknown.get()); }
    JSValue unknown() const { return m_unknown.get(); }
    // Of an operand that the operation leaves out: undefined.
    JSValue at(const Activation& activation, ValueID id) const
    {
        if (!id)
            return jsUndefined();
        JSValue value = activation.values[id.index()];
        return value ? value : unknown();
    }

    String charactersOf(StringID) const;
    JSValue textOf(StringID);
    // Null: no text names the property.
    Identifier nameOf(PropertyKeyID);
    JSValue builtinInRealm(BuiltinID);
    JSValue valueOfData(const DataNode&);
    JSValue listOf(std::span<const JSValue>);
    JSValue closureOf(FunctionID, std::span<const JSValue> captures);
    JSValue variable(VariableID);
    JSValue valueOfCommonJsModule(ModuleID);
    JSValue moduleObjectOf(ModuleID);
    JSValue namespaceOf(ModuleID);
    JSValue prototypeFor(JSValue newTarget);
    Identifier privateNameOf(LayoutID, uint32_t field);
    // No function: the value is no function of the program.
    ClosureOfProgram closureIn(JSValue) const;
    void givePrototypeTo(JSValue closure);
    bool isPatternMadeHere(JSValue value) const { return value.inherits<RegExpObject>() && asObject(value)->getDirect(*m_vm, m_markOfPatternMadeHere); }
    JSValue notePatternMadeHere(JSValue);
    // `Bun.serve({ fetch })`: what is handed to code that the rehearsal does not know may be called from there.
    void noteHandedToCodeThatNobodyKnows(std::span<const JSValue> arguments);
    std::optional<bool> isOfType(JSValue, TypeID) const;

    void noteTexts(std::span<const JSValue> arguments);
    JSValue call(JSValue callee, JSValue thisValue, std::span<const JSValue> arguments);
    JSValue construct(JSValue callee, JSValue newTarget, std::span<const JSValue> arguments);
    bool appendElementsOf(JSValue list, Values&);
    // False: the allowance does not pay for the call.
    bool payForBuiltin(JSValue thisValue, std::span<const JSValue> arguments);
    bool mayMatchWithoutEnd(RegExp*);
    void noteThrown(Exception*);
    void end()
    {
        m_operationsLeftInAll = 0;
        m_isOver = true;
    }

    JSValue execute(Activation&, const BodyValue&);
    // Whether the block is to be entered.
    bool takeEdge(Activation&, const BodyTerminator&, uint32_t targetIndex, BodyTarget);
    // The block where the ways meet again, if one of them got there.
    BlockID goEachWayFrom(Activation&, BodyBlock, JSValue& result);
    BlockID joinBehind(Activation&, BlockID);
    void runFrom(Activation&, BlockID, JSValue& result);
    void runOneOfWays(Activation&, BlockID, JSValue& result, uint32_t numberOfWaysLeft);
    JSValue runBlocks(Activation&);
    JSValue enter(FunctionID, std::span<const JSValue> captures, JSValue thisValue, JSValue callee, JSValue newTarget, std::span<const JSValue> arguments);
    JSValue enterWithNothingKnown(FunctionID);
    JSValue enterWithEntryParameters(FunctionID, std::span<const JSValue>);
    void runModule(ModuleID);
    // Whether a call of RegExp was handed a text that the rehearsal does not know.
    bool rehearse(FunctionID);

    const Program& m_program;
    const BodyIndexTable& m_bodies;
    const ThinSummaries& m_summaries;
    static constexpr uint32_t m_longestText = longestRegExpPatternWithCode;

    VM* const m_vm;
    Strong<JSGlobalObject> m_realm;
    Strong<JSObject> m_unknown;
    Strong<JSObject> m_absent;
    JSValue m_regExpConstructor;
    JSValue m_objectAssign;
    UncheckedKeyHashSet<JSCell*> m_functionsThatAskTheWorld;
    Values m_builtins;
    // By StringID: the place in m_textsMade.
    UncheckedKeyHashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_texts;
    Values m_textsMade;
    UncheckedKeyHashMap<uint32_t, Identifier, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_names;
    Values m_variables;
    Values m_resultsOfModules;
    Values m_moduleObjects;
    Values m_namespaces;
    // Functions of the program, in the order in which they were handed out, each once.
    Values m_handlers;
    UncheckedKeyHashSet<JSCell*> m_handlersListed;
    // A start that knows nothing of what it is handed hands functions to what it does not know all the time.
    bool m_goesThroughStartOfProgram { false };
    // Names that no code of the program can ask for.
    Identifier m_numberOfFunction;
    Identifier m_capturesOfFunction;
    Identifier m_markOfPatternMadeHere;
    UncheckedKeyHashMap<uint64_t, Identifier> m_privateNames;
    // By function: for each block the nearest block that every way from it to the end of the function goes through.
    UncheckedKeyHashMap<uint32_t, Vector<BlockID>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_joins;
    Vector<StateOfModule> m_statesOfModules;
    BitVector m_rehearsed;

    bool hasRoomLeft() const { return m_vm->heap.capacity() + m_charactersOfTextsMade <= mostBytesOfRealm; }

    uint32_t m_operationsLeft { 0 };
    uint64_t m_operationsLeftInAll { operationsInAllOfSmallestProgram };
    bool m_isOver { false };
    // Or at the start, of a start that has found none yet.
    uint64_t m_operationsLeftInAllAtLastText { 0 };
    size_t m_charactersOfTextsMade { 0 };
    unsigned m_depth { 0 };
    // In all the activations on the stack.
    unsigned m_openRegions { 0 };
    // By function. Of one that is being gone through: m_openRegions when it was last entered, and one more.
    Vector<unsigned> m_regionsOpenAtEntryPlusOne;
    bool m_sawTextThatNobodyKnows { false };

    UncheckedKeyHashMap<std::pair<String, uint32_t>, bool> m_patternsThatMayMatchWithoutEnd;
    UncheckedKeyHashSet<std::pair<String, uint32_t>> m_seen;
    Vector<RegExpTextWorkedOut> m_found;
};

RehearsalOfRegExpTexts::RehearsalOfRegExpTexts(VM& vm, const Program& program, const BodyIndexTable& bodies, const ThinSummaries& summaries)
    : m_program(program)
    , m_bodies(bodies)
    , m_summaries(summaries)
    , m_vm(&vm)
{
    m_realm.set(vm, JSGlobalObject::create(vm, JSGlobalObject::createStructure(vm, jsNull())));
    JSGlobalObject* realm = m_realm.get();

    m_unknown.set(vm, constructEmptyObject(vm, realm->nullPrototypeObjectStructure()));
    m_unknown->putDirect(vm, vm.propertyNames->toPrimitiveSymbol, JSNativeStdFunction::create(vm, realm, 0, String(), [](JSGlobalObject* realm, CallFrame*) {
        VM& vm = realm->vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        return throwVMTypeError(realm, scope, "nobody knows the value"_s);
    }));

    m_absent.set(vm, constructEmptyObject(vm, realm->nullPrototypeObjectStructure()));
    m_numberOfFunction = Identifier::fromUid(PrivateName(PrivateName::PrivateSymbol, "function"_s));
    m_capturesOfFunction = Identifier::fromUid(PrivateName(PrivateName::PrivateSymbol, "captures"_s));
    m_markOfPatternMadeHere = Identifier::fromUid(PrivateName(PrivateName::PrivateSymbol, "made"_s));

    const BuiltinTable& table = program.builtins();
    auto leaveEmpty = [](JSValue*) { };
    m_builtins.fill(vm, table.builtinCount(), leaveEmpty);
    m_variables.fill(vm, program.variables.size(), leaveEmpty);
    m_resultsOfModules.fill(vm, program.modules.size(), leaveEmpty);
    m_moduleObjects.fill(vm, program.modules.size(), leaveEmpty);
    m_namespaces.fill(vm, program.modules.size(), leaveEmpty);
    m_statesOfModules.fill(StateOfModule::IsNotEntered, program.modules.size());
    m_rehearsed.ensureSize(program.functions.size());
    m_regionsOpenAtEntryPlusOne.fill(0, program.functions.size());
    for (size_t function = 0; function < program.functions.size(); ++function)
        m_operationsLeftInAll += operationsInAllForEachOfProgram * summaries.at(FunctionID(static_cast<uint32_t>(function))).numberOfOperations;

    m_regExpConstructor = builtinInRealm(Builtins::RegExp);
    m_objectAssign = builtinInRealm(Builtins::ObjectAssign);
    for (unsigned index = 0; index < table.builtinCount(); ++index) {
        BuiltinID id(index);
        const Builtin& builtin = table.at(id);
        if (builtin.origin != BuiltinOrigin::Engine)
            continue;
        bool asksTheWorld = builtin.flags.containsAny({ BuiltinFlag::EvaluatesText, BuiltinFlag::LoadsModuleByName, BuiltinFlag::RunsUnseenCode, BuiltinFlag::StartsInstance });
        for (const BuiltinOverload& overload : table.overloadsOf(id))
            asksTheWorld |= overload.effects.reads.contains(AbstractHeap::ExternalState) || overload.effects.writes.contains(AbstractHeap::ExternalState);
        if (!asksTheWorld)
            continue;
        if (JSValue function = builtinInRealm(id); isKnown(function) && function.isCell())
            m_functionsThatAskTheWorld.add(function.asCell());
    }
}

String RehearsalOfRegExpTexts::charactersOf(StringID id) const
{
    Vector<char16_t> buffer;
    String characters = m_program.strings.viewOf(id, buffer).toString();
    return characters.is8Bit() || !characters.containsOnlyLatin1() ? characters : String::make8Bit(characters.span16());
}

JSValue RehearsalOfRegExpTexts::textOf(StringID id)
{
    return m_textsMade[m_texts.ensure(id.index(), [&] {
        m_textsMade.append(jsString(*m_vm, charactersOf(id)));
        return static_cast<uint32_t>(m_textsMade.size() - 1);
    }).iterator->value];
}

Identifier RehearsalOfRegExpTexts::nameOf(PropertyKeyID id)
{
    const PropertyKey& key = m_program.at(id);
    if (key.kind != PropertyKeyKind::Text || !key.text)
        return { };
    return m_names.ensure(id.index(), [&] {
        return Identifier::fromString(*m_vm, charactersOf(key.text));
    }).iterator->value;
}

JSValue RehearsalOfRegExpTexts::builtinInRealm(BuiltinID id)
{
    JSValue& found = m_builtins[id.index()];
    if (!found) {
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(*m_vm);
        auto cell = findInRealm(m_realm.get(), BuiltinCellKey::object(id));
        scope.clearException();
        found = cell && *cell ? JSValue(*cell) : unknown();
    }
    return found;
}

JSValue RehearsalOfRegExpTexts::listOf(std::span<const JSValue> elements)
{
    JSGlobalObject* realm = m_realm.get();
    JSArray* list = constructEmptyArray(realm, nullptr);
    for (JSValue element : elements)
        list->push(realm, element);
    return list;
}

JSValue RehearsalOfRegExpTexts::valueOfData(const DataNode& node)
{
    VM& vm = *m_vm;
    JSGlobalObject* realm = m_realm.get();
    if (node.is<DataNode::Undefined>())
        return jsUndefined();
    if (node.is<DataNode::Null>())
        return jsNull();
    if (const auto* boolean = node.tryAs<DataNode::Boolean>())
        return jsBoolean(boolean->value);
    if (const auto* number = node.tryAs<DataNode::Number>())
        return jsNumber(number->value);
    if (const auto* string = node.tryAs<DataNode::String>())
        return textOf(string->value);
    if (const auto* array = node.tryAs<DataNode::Array>()) {
        JSArray* list = constructEmptyArray(realm, nullptr);
        unsigned index = 0;
        for (const DataNode& element : array->elements) {
            if (!element.is<DataNode::Hole>())
                list->putDirectIndex(realm, index, valueOfData(element));
            ++index;
        }
        list->setLength(realm, index);
        return list;
    }
    if (const auto* object = node.tryAs<DataNode::Object>()) {
        JSObject* made = constructEmptyObject(realm);
        for (const DataMember& member : object->members) {
            if (Identifier name = nameOf(member.key); !name.isNull())
                made->putDirectMayBeIndex(realm, name, valueOfData(member.value));
        }
        return made;
    }
    UNUSED_PARAM(vm);
    return unknown();
}

JSValue RehearsalOfRegExpTexts::closureOf(FunctionID function, std::span<const JSValue> captures)
{
    const Function& record = m_program.at(function);
    // The function object keeps the list alive.
    VM& vm = *m_vm;
    JSGlobalObject* realm = m_realm.get();
    JSArray* held = asArray(listOf(captures));
    // A generator is a function like any other to whoever is handed it. What a call of it gives nobody knows.
    bool isGoneThrough = !record.has(FunctionFlags::IsGenerator) && record.hasBody();
    JSObject* made = JSNativeStdFunction::create(vm, realm, record.length, String(), [this, function, held, isGoneThrough](JSGlobalObject*, CallFrame* frame) {
        if (!isGoneThrough)
            return JSValue::encode(unknown());
        Values captures;
        appendElementsOf(held, captures);
        Values arguments;
        for (unsigned index = 0; index < frame->argumentCount(); ++index)
            arguments.append(frame->uncheckedArgument(index));
        return JSValue::encode(enter(function, captures.span(), frame->thisValue(), frame->jsCallee(), jsUndefined(), arguments.span()));
    }, held);
    made->putDirect(vm, m_numberOfFunction, jsNumber(function.index()));
    made->putDirect(vm, m_capturesOfFunction, held);
    return made;
}

RehearsalOfRegExpTexts::ClosureOfProgram RehearsalOfRegExpTexts::closureIn(JSValue value) const
{
    if (!value.inherits<JSNativeStdFunction>())
        return { };
    JSValue number = asObject(value)->getDirect(*m_vm, m_numberOfFunction);
    if (!number)
        return { };
    return { FunctionID(number.asUInt32()), asArray(asObject(value)->getDirect(*m_vm, m_capturesOfFunction)) };
}

// `function Layer() { }`, `Layer.prototype.match = ..`, `new Layer()`: made when somebody first asks.
void RehearsalOfRegExpTexts::givePrototypeTo(JSValue closure)
{
    VM& vm = *m_vm;
    FunctionID function = closureIn(closure).function;
    if (!function || asObject(closure)->getDirect(vm, vm.propertyNames->prototype))
        return;
    const Function& record = m_program.at(function);
    if (record.kind != FunctionKind::Plain || record.isCoroutine() || !record.hasBody())
        return;
    JSObject* prototype = constructEmptyObject(m_realm.get());
    prototype->putDirect(vm, vm.propertyNames->constructor, closure, static_cast<unsigned>(PropertyAttribute::DontEnum));
    asObject(closure)->putDirect(vm, vm.propertyNames->prototype, prototype, static_cast<unsigned>(PropertyAttribute::DontEnum));
}

JSValue RehearsalOfRegExpTexts::variable(VariableID id)
{
    JSValue& held = m_variables[id.index()];
    if (held)
        return held;
    const Variable& record = m_program.at(id);
    if (record.initialization == VariableInitialization::Function && record.function)
        return held = closureOf(record.function, { });
    runModule(record.module);
    // `held` stays where it is: the list never grows.
    if (held)
        return held;
    // `import express from "express"`: the loader fills the variable out of what the CommonJS module exports.
    const Module& module = m_program.at(record.module);
    for (const SyntheticExport& synthetic : module.syntheticExports) {
        if (synthetic.variable != id || module.requests.size() != 1)
            continue;
        JSValue exported = valueOfCommonJsModule(module.requests[0].target);
        if (!isKnown(exported))
            return unknown();
        String name = charactersOf(synthetic.name);
        if (name == "default"_s)
            return held = exported;
        if (!exported.isObject())
            return unknown();
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(*m_vm);
        JSValue member = exported.get(m_realm.get(), Identifier::fromString(*m_vm, name));
        if (scope.exception()) {
            scope.clearException();
            return unknown();
        }
        return held = member;
    }
    return record.initialization == VariableInitialization::Undefined && m_statesOfModules[record.module.index()] == StateOfModule::HasRun ? jsUndefined() : unknown();
}

JSValue RehearsalOfRegExpTexts::moduleObjectOf(ModuleID id)
{
    JSValue& held = m_moduleObjects[id.index()];
    if (held)
        return held;
    VM& vm = *m_vm;
    JSObject* made = constructEmptyObject(m_realm.get());
    const Module& module = m_program.at(id);
    JSValue exported = module.exportsVariable ? m_variables[module.exportsVariable.index()] : JSValue();
    made->putDirect(vm, Identifier::fromString(vm, "exports"_s), exported ? exported : JSValue(constructEmptyObject(m_realm.get())));
    return held = made;
}

JSValue RehearsalOfRegExpTexts::valueOfCommonJsModule(ModuleID id)
{
    runModule(id);
    const Module& module = m_program.at(id);
    if (JSValue object = m_moduleObjects[id.index()])
        return asObject(object)->getDirect(*m_vm, Identifier::fromString(*m_vm, "exports"_s));
    if (!module.exportsVariable)
        return unknown();
    JSValue exported = m_variables[module.exportsVariable.index()];
    return exported ? exported : unknown();
}

JSValue RehearsalOfRegExpTexts::namespaceOf(ModuleID id)
{
    VM& vm = *m_vm;
    if (JSValue held = m_namespaces[id.index()])
        return held;
    runModule(id);
    const Module& module = m_program.at(id);
    JSObject* made = constructEmptyObject(m_realm.get());
    for (const ModuleExport& exported : module.exports) {
        if (exported.kind == ModuleExportKind::Variable && exported.variable)
            made->putDirect(vm, Identifier::fromString(vm, charactersOf(exported.name)), variable(exported.variable));
    }
    for (const SyntheticExport& synthetic : module.syntheticExports) {
        if (synthetic.variable)
            made->putDirect(vm, Identifier::fromString(vm, charactersOf(synthetic.name)), variable(synthetic.variable));
    }
    // What is asked for while the body runs holds what the body has stored so far.
    if (m_statesOfModules[id.index()] == StateOfModule::HasRun)
        m_namespaces[id.index()] = made;
    return made;
}

JSValue RehearsalOfRegExpTexts::prototypeFor(JSValue newTarget)
{
    JSGlobalObject* realm = m_realm.get();
    if (!isKnown(newTarget) || !newTarget.isObject())
        return realm->objectPrototype();
    givePrototypeTo(newTarget);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(*m_vm);
    JSValue prototype = newTarget.get(realm, m_vm->propertyNames->prototype);
    if (scope.exception())
        scope.clearException();
    else if (isKnown(prototype) && prototype.isObject())
        return prototype;
    return realm->objectPrototype();
}

Identifier RehearsalOfRegExpTexts::privateNameOf(LayoutID layout, uint32_t field)
{
    return m_privateNames.ensure((static_cast<uint64_t>(layout.index()) + 1) << 32 | field, [&] {
        return Identifier::fromUid(PrivateName(PrivateName::PrivateSymbol, "field"_s));
    }).iterator->value;
}

JSValue RehearsalOfRegExpTexts::notePatternMadeHere(JSValue made)
{
    if (made.inherits<RegExpObject>())
        asObject(made)->putDirect(*m_vm, m_markOfPatternMadeHere, jsBoolean(true));
    return made;
}

std::optional<bool> RehearsalOfRegExpTexts::isOfType(JSValue value, TypeID id) const
{
    Type type = m_program.at(id);
    if (!type.isJavaScriptValue())
        return std::nullopt;
    auto has = [&](auto primitives) {
        return !!(type.primitives & primitives);
    };
    if (value == JSValue(m_absent.get()))
        return has(Primitive::Absent);
    if (value.isUndefined())
        return has(Primitive::Undefined);
    if (value.isNull())
        return has(Primitive::Null);
    if (value.isBoolean())
        return has(value.asBoolean() ? Primitive::True : Primitive::False);
    if (value.isNumber()) {
        if (!has(Primitive::Number))
            return false;
        if ((type.primitives & Primitive::Number) == Primitive::Number && type.integers == Range::full() && type.boundsOfFractions.isEmpty())
            return true;
        return std::nullopt;
    }
    if (value.isString()) {
        if (has(Primitive::String))
            return true;
        if (!has(Primitive::LiteralString))
            return false;
        return std::nullopt;
    }
    if (value.isSymbol())
        return has(Primitive::Symbol) ? std::optional(true) : has(Primitive::LiteralSymbol) ? std::nullopt : std::optional(false);
    if (value.isBigInt())
        return has(Primitive::BigInt);
    if (!value.isObject())
        return std::nullopt;
    if (!type.hasObjects())
        return false;
    bool isCallable = value.isCallable();
    if (isCallable ? type.anyFunction : type.anyObject)
        return true;
    if (!isCallable && !type.anyObject && !type.objects && !type.unnamedLayouts && !type.unnamedClasses)
        return false;
    return std::nullopt;
}

// ---- Calls

void RehearsalOfRegExpTexts::noteTexts(std::span<const JSValue> arguments)
{
    JSGlobalObject* realm = m_realm.get();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(*m_vm);
    JSValue pattern = arguments.size() > 0 ? arguments[0] : jsUndefined();
    JSValue flags = arguments.size() > 1 ? arguments[1] : jsUndefined();
    if (!isKnown(pattern)) {
        m_sawTextThatNobodyKnows = true;
        return;
    }

    String text = emptyString();
    std::optional<OptionSet<Yarr::Flags>> flagsOfText = OptionSet<Yarr::Flags>();
    if (auto* copied = dynamicDowncast<RegExpObject>(pattern)) {
        text = copied->regExp()->pattern();
        flagsOfText = copied->regExp()->flags();
    } else if (!pattern.isUndefined())
        text = pattern.toWTFString(realm);
    if (scope.exception()) {
        scope.clearException();
        m_sawTextThatNobodyKnows = true;
        return;
    }

    bool knowsFlags = isKnown(flags);
    if (!knowsFlags) {
        // They may be "", "g" or "d", which all find the record without significant flags.
        m_sawTextThatNobodyKnows = true;
        flagsOfText = OptionSet<Yarr::Flags>();
    } else if (!flags.isUndefined()) {
        String written = flags.toWTFString(realm);
        if (scope.exception()) {
            scope.clearException();
            return;
        }
        flagsOfText = Yarr::parseFlags(written);
    }
    if (!flagsOfText || text.isNull() || text.length() > m_longestText)
        return;

    uint16_t significant = ImageRegExp::significantFlags(*flagsOfText).toRaw();
    if (m_seen.add({ text, significant }).isNewEntry) {
        m_found.append(RegExpTextWorkedOut { text.isolatedCopy(), significant, knowsFlags });
        // What has led to a text was not gone through for nothing: it is given back, and the text costs what it costs. So a start goes on for as long as it finds texts.
        m_operationsLeft = std::max(m_operationsLeft, operationsFromOneStart);
        if (m_operationsLeftInAll) {
            m_operationsLeftInAll = m_operationsLeftInAllAtLastText > operationsForText ? m_operationsLeftInAllAtLastText - operationsForText : 0;
            m_operationsLeftInAllAtLastText = m_operationsLeftInAll;
        }
    }
}

void RehearsalOfRegExpTexts::noteHandedToCodeThatNobodyKnows(std::span<const JSValue> arguments)
{
    if (!m_goesThroughStartOfProgram)
        return;
    VM& vm = *m_vm;
    auto list = [&](JSValue value) {
        if (closureIn(value).function && m_handlersListed.add(value.asCell()).isNewEntry)
            m_handlers.append(value);
    };
    for (JSValue argument : arguments) {
        list(argument);
        if (!isKnown(argument) || !argument.inherits<JSFinalObject>())
            continue;
        JSObject* options = asObject(argument);
        options->structure()->forEachProperty(vm, [&](const PropertyTableEntry& entry) {
            list(options->getDirect(entry.offset()));
            return true;
        });
    }
}

JSValue RehearsalOfRegExpTexts::call(JSValue callee, JSValue thisValue, std::span<const JSValue> arguments)
{
    VM& vm = *m_vm;
    JSGlobalObject* realm = m_realm.get();
    if (!isKnown(callee) || !callee.isCell() || m_functionsThatAskTheWorld.contains(callee.asCell())) {
        noteHandedToCodeThatNobodyKnows(arguments);
        return unknown();
    }
    if (callee == m_regExpConstructor)
        noteTexts(arguments);
    bool isOfProgram = callee.inherits<JSNativeStdFunction>();
    if (!isOfProgram && !isKnown(thisValue))
        return unknown();
    // A function that is handed nothing that anybody knows does what it does from a start of its own, if it makes patterns.
    if (isOfProgram && !arguments.empty() && (!isKnown(thisValue) || thisValue.isUndefined()) && std::ranges::none_of(arguments, [&](JSValue argument) { return isKnown(argument); }))
        return unknown();
    // What a program does with the patterns that it has made is its work, not the making of patterns.
    if (!isOfProgram && callee != m_regExpConstructor && (isPatternMadeHere(thisValue) || std::ranges::any_of(arguments, [&](JSValue argument) { return isPatternMadeHere(argument); })))
        return unknown();
    CallData callData = getCallData(callee);
    if (callData.type == CallData::Type::None)
        return unknown();

    if (!isOfProgram && !payForBuiltin(thisValue, arguments))
        return unknown();
    MarkedArgumentBuffer list;
    for (JSValue argument : arguments)
        list.append(argument);
    if (list.hasOverflowed())
        return unknown();

    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    NakedPtr<Exception> exception;
    JSValue result = JSC::call(realm, callee, callData, thisValue, list, exception);
    scope.clearException();
    if (exception)
        noteThrown(exception.get());
    if (exception || !result)
        return unknown();
    return callee == m_regExpConstructor ? notePatternMadeHere(result) : result;
}

bool RehearsalOfRegExpTexts::mayMatchWithoutEnd(RegExp* regExp)
{
    return m_patternsThatMayMatchWithoutEnd.ensure({ regExp->pattern(), regExp->flags().toRaw() }, [&] {
        Yarr::ErrorCode error = Yarr::ErrorCode::NoError;
        Yarr::YarrPattern pattern(regExp->pattern(), regExp->flags(), error);
        return Yarr::hasError(error) || repeatsInsideRepeat(*pattern.m_body, false);
    }).iterator->value;
}

bool RehearsalOfRegExpTexts::payForBuiltin(JSValue thisValue, std::span<const JSValue> arguments)
{
    uint64_t handed = 0;
    uint64_t characters = 0;
    bool mayMatch = false;
    auto take = [&](JSValue value) {
        if (value.isString())
            characters += asString(value)->length();
        else if (auto* list = dynamicDowncast<JSArray>(value))
            handed += list->length();
        else if (auto* pattern = dynamicDowncast<RegExpObject>(value)) {
            if (mayMatchWithoutEnd(pattern->regExp()))
                return false;
            mayMatch = true;
        }
        // A number may set how much the built-in allocates.
        return !value.isNumber() || value.asNumber() <= m_longestText;
    };
    if (!take(thisValue) || !std::ranges::all_of(arguments, take))
        return false;
    uint64_t price = mayMatch ? operationsForBuiltinHandedRegExp + characters : handed + characters;
    if (price >= m_operationsLeft || price >= m_operationsLeftInAll)
        return false;
    m_operationsLeft -= static_cast<uint32_t>(price);
    m_operationsLeftInAll -= price;
    return true;
}

void RehearsalOfRegExpTexts::noteThrown(Exception* exception)
{
    auto* error = dynamicDowncast<ErrorInstance>(exception->value());
    if (m_vm->isTerminationException(exception) || (error && (error->isOutOfMemoryError() || error->isStackOverflowError())))
        end();
}

JSValue RehearsalOfRegExpTexts::construct(JSValue callee, JSValue newTarget, std::span<const JSValue> arguments)
{
    VM& vm = *m_vm;
    JSGlobalObject* realm = m_realm.get();
    if (!isKnown(callee) || !callee.isCell() || m_functionsThatAskTheWorld.contains(callee.asCell()))
        return unknown();
    if (!isKnown(newTarget) || !newTarget.isObject())
        newTarget = callee;
    if (ClosureOfProgram closure = closureIn(callee); closure.function) {
        FunctionID function = closure.function;
        if (m_program.at(function).isCoroutine() || !m_program.at(function).hasBody())
            return unknown();
        Values captures;
        appendElementsOf(closure.captures, captures);
        switch (m_program.at(function).kind) {
        case FunctionKind::BaseConstructor:
        case FunctionKind::DerivedConstructor:
            return enter(function, captures.span(), jsUndefined(), callee, newTarget, arguments);
        case FunctionKind::Plain: {
            JSObject* made = constructEmptyObject(realm, asObject(prototypeFor(newTarget)));
            JSValue returned = enter(function, captures.span(), made, callee, newTarget, arguments);
            return isKnown(returned) && returned.isObject() ? returned : JSValue(made);
        }
        default:
            return unknown();
        }
    }
    if (callee.inherits<JSNativeStdFunction>())
        return unknown();
    if (callee == m_regExpConstructor)
        noteTexts(arguments);
    CallData constructData = getConstructData(callee);
    if (constructData.type == CallData::Type::None)
        return unknown();

    if (!payForBuiltin(jsUndefined(), arguments))
        return unknown();
    MarkedArgumentBuffer list;
    for (JSValue argument : arguments)
        list.append(argument);
    if (list.hasOverflowed())
        return unknown();

    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSObject* result = JSC::construct(realm, callee, constructData, list, newTarget);
    if (Exception* exception = scope.exception()) {
        noteThrown(exception);
        scope.clearException();
        return unknown();
    }
    if (!result)
        return unknown();
    return callee == m_regExpConstructor ? notePatternMadeHere(result) : JSValue(result);
}

bool RehearsalOfRegExpTexts::appendElementsOf(JSValue list, Values& elements)
{
    JSGlobalObject* realm = m_realm.get();
    auto* array = dynamicDowncast<JSArray>(list);
    if (!array || array->length() > m_longestText)
        return false;
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(*m_vm);
    for (unsigned index = 0; index < array->length(); ++index) {
        JSValue element = array->getIndex(realm, index);
        if (scope.exception()) {
            scope.clearException();
            return false;
        }
        elements.append(element);
    }
    return true;
}

// ---- One operation

JSValue RehearsalOfRegExpTexts::execute(Activation& activation, const BodyValue& value)
{
    VM& vm = *m_vm;
    JSGlobalObject* realm = m_realm.get();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    auto operand = [&](ValueID id) {
        return at(activation, id);
    };
    auto settle = [&](JSValue result) -> JSValue {
        if (Exception* exception = scope.exception()) {
            noteThrown(exception);
            scope.clearException();
            return unknown();
        }
        if (!result || (result.isString() && asString(result)->length() > m_longestText))
            return unknown();
        if (result.isString())
            m_charactersOfTextsMade += asString(result)->length();
        return result;
    };
    auto operandsOf = [&](std::span<const ValueID> ids) {
        Values values;
        for (ValueID id : ids)
            values.append(operand(id));
        return values;
    };
    auto number = [&](JSValue of) {
        return of.toNumber(realm);
    };

    Opcode opcode = value.opcode();
    switch (opcode) {
    case Opcode::Undefined:
        return jsUndefined();
    case Opcode::Null:
        return jsNull();
    case Opcode::Absent:
        return m_absent.get();
    case Opcode::Boolean:
        return jsBoolean(value.as<Opcode::Boolean>().boolean());
    case Opcode::Integer:
        return jsNumber(static_cast<double>(value.as<Opcode::Integer>().integer()));
    case Opcode::Double:
        return jsNumber(value.as<Opcode::Double>().number());
    case Opcode::String:
        return textOf(value.as<Opcode::String>().string());
    case Opcode::Builtin:
        return builtinInRealm(value.as<Opcode::Builtin>().builtin());
    case Opcode::Data:
        return settle(valueOfData(m_program.at(value.as<Opcode::Data>().data()).root));
    case Opcode::NewClosure: {
        auto closure = value.as<Opcode::NewClosure>();
        return closureOf(closure.function(), operandsOf(closure.capturesOperands()).span());
    }
    case Opcode::GetModuleVariable:
        return variable(value.as<Opcode::GetModuleVariable>().variable());
    case Opcode::PutModuleVariable: {
        auto store = value.as<Opcode::PutModuleVariable>();
        m_variables[store.variable().index()] = operand(store.valueOperand());
        return { };
    }

    case Opcode::Upcast:
    case Opcode::NarrowByField:
    case Opcode::NarrowByTest:
    case Opcode::Check:
    case Opcode::Convert:
    case Opcode::RequireObjectCoercible:
    case Opcode::RequireObject:
    case Opcode::FinishObject:
    case Opcode::FinishClass:
        return operand(value.operand(0));
    case Opcode::CheckInitialized: {
        JSValue held = operand(value.operand(0));
        return held == JSValue(m_absent.get()) ? unknown() : held;
    }
    case Opcode::LoopHint:
    case Opcode::ReleaseHint:
    case Opcode::RecordRequire:
    case Opcode::SetFunctionName:
    case Opcode::DefineSetter:
    case Opcode::IteratorClose:
        return { };

    case Opcode::NewObject: {
        auto allocation = value.as<Opcode::NewObject>();
        const Layout& layout = m_program.at(allocation.layout());
        JSValue prototype = allocation.prototypeOperand() ? operand(allocation.prototypeOperand()) : JSValue();
        JSObject* made = prototype && isKnown(prototype) && prototype.isObject() ? constructEmptyObject(realm, asObject(prototype)) : constructEmptyObject(realm);
        std::span<const ValueID> values = allocation.valuesOperands();
        unsigned index = 0;
        for (auto field : allocation.fields()) {
            if (index >= values.size())
                break;
            JSValue stored = operand(values[index++]);
            if (isEnvironmentKind(layout.kind))
                made->putDirectIndex(realm, static_cast<unsigned>(field), stored);
            else if (PropertyKeyID key = layout.fields[static_cast<unsigned>(field)].key) {
                if (Identifier name = nameOf(key); !name.isNull())
                    made->putDirectMayBeIndex(realm, name, stored);
            }
        }
        return settle(made);
    }
    case Opcode::NewDictionary:
        return constructEmptyObject(realm);
    case Opcode::GetEnvironmentField: {
        auto read = value.as<Opcode::GetEnvironmentField>();
        JSValue environment = operand(read.environmentOperand());
        if (!isKnown(environment) || !environment.isObject())
            return unknown();
        JSValue held = asObject(environment)->getDirectIndex(realm, static_cast<unsigned>(read.field()));
        return settle(held);
    }
    case Opcode::PutEnvironmentField: {
        auto store = value.as<Opcode::PutEnvironmentField>();
        JSValue environment = operand(store.environmentOperand());
        if (isKnown(environment) && environment.isObject())
            asObject(environment)->putDirectIndex(realm, static_cast<unsigned>(store.field()), operand(store.valueOperand()));
        scope.clearException();
        return { };
    }

    case Opcode::NewArray: {
        auto allocation = value.as<Opcode::NewArray>();
        JSArray* list = constructEmptyArray(realm, nullptr);
        auto holes = allocation.holes();
        auto nextHole = holes.begin();
        unsigned index = 0;
        for (ValueID element : allocation.elementsOperands()) {
            while (nextHole != holes.end() && static_cast<unsigned>(*nextHole) == index) {
                ++index;
                ++nextHole;
            }
            list->putDirectIndex(realm, index++, operand(element));
        }
        for (; nextHole != holes.end(); ++nextHole)
            ++index;
        list->setLength(realm, index);
        return settle(list);
    }
    case Opcode::AppendElement: {
        auto append = value.as<Opcode::AppendElement>();
        if (auto* list = dynamicDowncast<JSArray>(operand(append.arrayOperand())))
            list->push(realm, operand(append.valueOperand()));
        scope.clearException();
        return { };
    }
    case Opcode::AppendHole: {
        if (auto* list = dynamicDowncast<JSArray>(operand(value.operand(0))))
            list->setLength(realm, list->length() + 1);
        scope.clearException();
        return { };
    }
    case Opcode::AppendSpread: {
        auto append = value.as<Opcode::AppendSpread>();
        auto* list = dynamicDowncast<JSArray>(operand(append.arrayOperand()));
        JSValue iterable = operand(append.iterableOperand());
        if (!list)
            return { };
        if (!isKnown(iterable)) {
            list->push(realm, unknown());
            scope.clearException();
            return { };
        }
        forEachInIterable(realm, iterable, [&](VM&, JSGlobalObject* realm, JSValue element) {
            list->push(realm, element);
        });
        scope.clearException();
        return { };
    }
    case Opcode::NewRegExp: {
        auto literal = value.as<Opcode::NewRegExp>();
        RegExp* regExp = RegExp::create(vm, charactersOf(literal.pattern()), toYarrFlags(literal.regExpFlags()));
        if (!regExp->isValid())
            return unknown();
        return RegExpObject::create(vm, realm->regExpStructure(), regExp);
    }
    case Opcode::NewTemplateObject: {
        JSArray* cooked = constructEmptyArray(realm, nullptr);
        JSArray* raw = constructEmptyArray(realm, nullptr);
        for (TemplateString part : value.as<Opcode::NewTemplateObject>().templateStrings()) {
            cooked->push(realm, part.cooked ? textOf(part.cooked) : jsUndefined());
            raw->push(realm, textOf(part.raw));
        }
        cooked->putDirect(vm, vm.propertyNames->raw, raw);
        return settle(cooked);
    }

    case Opcode::PropGet: {
        auto read = value.as<Opcode::PropGet>();
        JSValue object = operand(read.objectOperand());
        Identifier name = nameOf(read.key());
        if (!isKnown(object) || object.isUndefinedOrNull() || name.isNull())
            return unknown();
        if (name == vm.propertyNames->prototype)
            givePrototypeTo(object);
        JSValue found = settle(object.get(realm, name));
        // The realm of the build has none of what the host lays on the global object.
        if (found.isUndefined() && object == JSValue(realm))
            return unknown();
        return found;
    }
    case Opcode::ElemGet: {
        auto read = value.as<Opcode::ElemGet>();
        JSValue object = operand(read.objectOperand());
        JSValue property = operand(read.propertyOperand());
        if (!isKnown(object) || !isKnown(property) || object.isUndefinedOrNull())
            return unknown();
        Identifier name = property.toPropertyKey(realm);
        if (scope.exception())
            return settle({ });
        return settle(object.get(realm, name));
    }
    case Opcode::PropSet:
    case Opcode::PropDefine: {
        JSValue object = operand(value.operand(0));
        PropertyKeyID key = opcode == Opcode::PropSet ? value.as<Opcode::PropSet>().key() : value.as<Opcode::PropDefine>().key();
        JSValue stored = operand(opcode == Opcode::PropSet ? value.as<Opcode::PropSet>().valueOperand() : value.as<Opcode::PropDefine>().valueOperand());
        Identifier name = nameOf(key);
        if (!isKnown(object) || !object.isObject() || object == JSValue(realm) || name.isNull())
            return { };
        if (opcode == Opcode::PropDefine)
            asObject(object)->putDirectMayBeIndex(realm, name, stored);
        else {
            PutPropertySlot slot(object);
            object.put(realm, name, stored, slot);
        }
        scope.clearException();
        return { };
    }
    case Opcode::ElemSet:
    case Opcode::ElemDefine: {
        JSValue object = operand(value.operand(0));
        JSValue property = operand(value.operand(1));
        JSValue stored = operand(opcode == Opcode::ElemSet ? value.as<Opcode::ElemSet>().valueOperand() : value.as<Opcode::ElemDefine>().valueOperand());
        if (!isKnown(object) || !isKnown(property) || !object.isObject() || object == JSValue(realm))
            return { };
        Identifier name = property.toPropertyKey(realm);
        if (!scope.exception()) {
            PutPropertySlot slot(object);
            object.put(realm, name, stored, slot);
        }
        scope.clearException();
        return { };
    }
    case Opcode::DefineGetter: {
        // What a read of the name gives is what the function answers then.
        auto definition = value.as<Opcode::DefineGetter>();
        JSValue object = operand(definition.objectOperand());
        Identifier name = definition.key() ? nameOf(definition.key()) : Identifier();
        if (isKnown(object) && object.isObject() && object != JSValue(realm) && !name.isNull())
            asObject(object)->putDirectMayBeIndex(realm, name, unknown());
        scope.clearException();
        return { };
    }
    case Opcode::ElemHas: {
        auto test = value.as<Opcode::ElemHas>();
        JSValue object = operand(test.objectOperand());
        JSValue property = operand(test.propertyOperand());
        if (!isKnown(object) || !isKnown(property) || !object.isObject() || object == JSValue(realm))
            return unknown();
        Identifier name = property.toPropertyKey(realm);
        if (scope.exception())
            return settle({ });
        return settle(jsBoolean(asObject(object)->hasProperty(realm, name)));
    }
    case Opcode::PropDelete:
    case Opcode::ElemDelete: {
        JSValue object = operand(value.operand(0));
        if (!isKnown(object) || !object.isObject() || object == JSValue(realm))
            return unknown();
        Identifier name;
        if (opcode == Opcode::PropDelete)
            name = nameOf(value.as<Opcode::PropDelete>().key());
        else if (JSValue property = operand(value.as<Opcode::ElemDelete>().propertyOperand()); isKnown(property))
            name = property.toPropertyKey(realm);
        if (scope.exception() || name.isNull())
            return settle({ });
        return settle(jsBoolean(JSCell::deleteProperty(asObject(object), realm, name)));
    }
    case Opcode::SetLiteralPrototype: {
        auto store = value.as<Opcode::SetLiteralPrototype>();
        JSValue object = operand(store.objectOperand());
        JSValue prototype = operand(store.prototypeOperand());
        if (isKnown(object) && object.isObject() && isKnown(prototype) && (prototype.isObject() || prototype.isNull()))
            asObject(object)->setPrototype(vm, realm, prototype);
        scope.clearException();
        return { };
    }
    case Opcode::PropHas: {
        auto test = value.as<Opcode::PropHas>();
        JSValue object = operand(test.objectOperand());
        Identifier name = nameOf(test.key());
        if (!isKnown(object) || !object.isObject() || object == JSValue(realm) || name.isNull())
            return unknown();
        return settle(jsBoolean(asObject(object)->hasProperty(realm, name)));
    }
    case Opcode::CopyDataProperties:
    case Opcode::CopyObject: {
        bool copiesInto = opcode == Opcode::CopyDataProperties;
        JSValue target = copiesInto ? operand(value.as<Opcode::CopyDataProperties>().targetOperand()) : JSValue(constructEmptyObject(realm));
        JSValue source = operand(copiesInto ? value.as<Opcode::CopyDataProperties>().sourceOperand() : value.as<Opcode::CopyObject>().sourceOperand());
        bool leavesSomeOut = copiesInto ? !value.as<Opcode::CopyDataProperties>().excludedKeys().isEmpty() || !value.as<Opcode::CopyDataProperties>().excludedOperands().empty()
            : !value.as<Opcode::CopyObject>().excludedKeys().isEmpty() || !value.as<Opcode::CopyObject>().excludedOperands().empty();
        if (!isKnown(target) || !isKnown(source) || leavesSomeOut)
            return copiesInto ? JSValue() : unknown();
        JSValue arguments[] { target, source };
        call(m_objectAssign, jsUndefined(), arguments);
        return copiesInto ? JSValue() : target;
    }

    case Opcode::Add: {
        JSValue left = operand(value.operand(0));
        JSValue right = operand(value.operand(1));
        if (!isKnown(left) || !isKnown(right))
            return unknown();
        return settle(jsAdd(realm, left, right));
    }
    case Opcode::Sub:
    case Opcode::Mul:
    case Opcode::Div:
    case Opcode::Mod:
    case Opcode::Pow:
    case Opcode::BitAnd:
    case Opcode::BitOr:
    case Opcode::BitXor:
    case Opcode::LShift:
    case Opcode::RShift:
    case Opcode::URShift:
    case Opcode::Less:
    case Opcode::LessEq:
    case Opcode::Greater:
    case Opcode::GreaterEq:
    case Opcode::Eq:
    case Opcode::StrictEq: {
        JSValue left = operand(value.operand(0));
        JSValue right = operand(value.operand(1));
        if (!isKnown(left) || !isKnown(right))
            return unknown();
        bool isArithmetic = opcode != Opcode::Less && opcode != Opcode::LessEq && opcode != Opcode::Greater && opcode != Opcode::GreaterEq && opcode != Opcode::Eq && opcode != Opcode::StrictEq;
        if (isArithmetic && (left.isBigInt() || right.isBigInt() || left.isObject() || right.isObject()))
            return unknown();
        switch (opcode) {
        case Opcode::Mod:
            return settle(jsNumber(fmod(number(left), number(right))));
        case Opcode::Pow:
            return settle(jsNumber(operationMathPow(number(left), number(right))));
        case Opcode::BitAnd:
            return settle(jsNumber(left.toInt32(realm) & right.toInt32(realm)));
        case Opcode::BitOr:
            return settle(jsNumber(left.toInt32(realm) | right.toInt32(realm)));
        case Opcode::BitXor:
            return settle(jsNumber(left.toInt32(realm) ^ right.toInt32(realm)));
        case Opcode::LShift:
            return settle(jsNumber(static_cast<int32_t>(static_cast<uint32_t>(left.toInt32(realm)) << (right.toUInt32(realm) & 31))));
        case Opcode::RShift:
            return settle(jsNumber(left.toInt32(realm) >> (right.toUInt32(realm) & 31)));
        case Opcode::URShift:
            return settle(jsNumber(left.toUInt32(realm) >> (right.toUInt32(realm) & 31)));
        case Opcode::Sub:
            return settle(jsNumber(number(left) - number(right)));
        case Opcode::Mul:
            return settle(jsNumber(number(left) * number(right)));
        case Opcode::Div:
            return settle(jsNumber(number(left) / number(right)));
        case Opcode::Less:
            return settle(jsBoolean(jsLess<true>(realm, left, right)));
        case Opcode::LessEq:
            return settle(jsBoolean(jsLessEq<true>(realm, left, right)));
        case Opcode::Greater:
            return settle(jsBoolean(jsLess<false>(realm, right, left)));
        case Opcode::GreaterEq:
            return settle(jsBoolean(jsLessEq<false>(realm, right, left)));
        case Opcode::Eq:
            return settle(jsBoolean(JSValue::equal(realm, left, right)));
        default:
            return settle(jsBoolean(JSValue::strictEqual(realm, left, right)));
        }
    }
    case Opcode::ToNumber:
    case Opcode::ToNumeric:
    case Opcode::Increment:
    case Opcode::Decrement:
    case Opcode::Negate: {
        JSValue of = operand(value.operand(0));
        if (!isKnown(of) || of.isBigInt())
            return unknown();
        double converted = number(of);
        return settle(jsNumber(opcode == Opcode::Increment ? converted + 1 : opcode == Opcode::Decrement ? converted - 1 : opcode == Opcode::Negate ? -converted : converted));
    }
    case Opcode::BitNot: {
        JSValue of = operand(value.operand(0));
        return isKnown(of) && !of.isBigInt() && !of.isObject() ? settle(jsNumber(~of.toInt32(realm))) : unknown();
    }
    case Opcode::ToObject: {
        JSValue of = operand(value.operand(0));
        return isKnown(of) && !of.isUndefinedOrNull() ? settle(of.toObject(realm)) : unknown();
    }
    case Opcode::Await: {
        JSValue awaited = operand(value.operand(0));
        if (!isKnown(awaited))
            return unknown();
        if (auto* promise = dynamicDowncast<JSPromise>(awaited))
            return promise->status() == JSPromise::Status::Fulfilled ? promise->result() : unknown();
        return awaited.isObject() ? unknown() : awaited;
    }
    case Opcode::ToThis: {
        JSValue of = operand(value.operand(0));
        if (!isKnown(of))
            return unknown();
        return of.isUndefinedOrNull() ? JSValue(realm) : settle(of.toObject(realm));
    }
    case Opcode::ToLength: {
        JSValue of = operand(value.operand(0));
        return isKnown(of) && !of.isObject() ? settle(jsNumber(of.toLength(realm))) : unknown();
    }
    case Opcode::ToBoolean: {
        JSValue of = operand(value.operand(0));
        return isKnown(of) ? jsBoolean(of.toBoolean(realm)) : unknown();
    }
    case Opcode::LogicalNot: {
        JSValue of = operand(value.operand(0));
        return isKnown(of) ? jsBoolean(!of.toBoolean(realm)) : unknown();
    }
    case Opcode::TypeOf: {
        JSValue of = operand(value.operand(0));
        return isKnown(of) ? JSValue(jsTypeStringForValue(realm, of)) : unknown();
    }
    case Opcode::TypeTest: {
        auto test = value.as<Opcode::TypeTest>();
        JSValue of = operand(test.valueOperand());
        if (!isKnown(of))
            return unknown();
        std::optional<bool> answer = isOfType(of, test.testedType());
        return answer ? jsBoolean(*answer) : unknown();
    }
    case Opcode::InstanceOf: {
        JSValue of = operand(value.operand(0));
        JSValue constructor = operand(value.operand(1));
        if (!isKnown(of) || !isKnown(constructor) || !constructor.isObject())
            return unknown();
        givePrototypeTo(constructor);
        return settle(jsBoolean(asObject(constructor)->hasInstance(realm, of)));
    }
    case Opcode::ToString: {
        JSValue of = operand(value.operand(0));
        return isKnown(of) ? settle(of.toString(realm)) : unknown();
    }
    case Opcode::ToPropertyKey: {
        JSValue of = operand(value.operand(0));
        return isKnown(of) && !of.isObject() ? of : unknown();
    }
    case Opcode::Strcat: {
        StringBuilder builder;
        for (ValueID part : value.as<Opcode::Strcat>().partsOperands()) {
            JSValue of = operand(part);
            if (!isKnown(of))
                return unknown();
            builder.append(of.toWTFString(realm));
            if (scope.exception() || builder.length() > m_longestText)
                return settle({ });
        }
        return settle(jsString(vm, builder.toString()));
    }

    case Opcode::Call: {
        auto written = value.as<Opcode::Call>();
        return settle(call(operand(written.calleeOperand()), operand(written.thisValueOperand()), operandsOf(written.argumentsOperands()).span()));
    }
    case Opcode::Construct: {
        auto written = value.as<Opcode::Construct>();
        return settle(construct(operand(written.calleeOperand()), operand(written.newTargetOperand()), operandsOf(written.argumentsOperands()).span()));
    }
    case Opcode::CallList: {
        auto written = value.as<Opcode::CallList>();
        Values arguments;
        if (!appendElementsOf(operand(written.listOperand()), arguments))
            return unknown();
        return settle(call(operand(written.calleeOperand()), operand(written.thisValueOperand()), arguments.span()));
    }
    case Opcode::ConstructList: {
        auto written = value.as<Opcode::ConstructList>();
        Values arguments;
        if (!appendElementsOf(operand(written.listOperand()), arguments))
            return unknown();
        return settle(construct(operand(written.calleeOperand()), operand(written.newTargetOperand()), arguments.span()));
    }

    case Opcode::CreateUnmappedArguments:
    case Opcode::CreateMappedArguments: {
        bool isMapped = opcode == Opcode::CreateMappedArguments;
        JSValue count = operand(isMapped ? value.as<Opcode::CreateMappedArguments>().countOperand() : value.as<Opcode::CreateUnmappedArguments>().countOperand());
        JSValue rest = operand(isMapped ? value.as<Opcode::CreateMappedArguments>().restOperand() : value.as<Opcode::CreateUnmappedArguments>().restOperand());
        std::span<const ValueID> parameters = isMapped ? value.as<Opcode::CreateMappedArguments>().parametersOperands() : value.as<Opcode::CreateUnmappedArguments>().parametersOperands();
        if (!isKnown(count) || !count.isNumber())
            return unknown();
        Values all;
        for (size_t index = 0; index < parameters.size() && index < count.asNumber(); ++index)
            all.append(operand(parameters[index]));
        if (!appendElementsOf(rest, all))
            return unknown();
        return settle(listOf(all.span()));
    }
    case Opcode::GetMappedArgument: {
        auto read = value.as<Opcode::GetMappedArgument>();
        auto* list = dynamicDowncast<JSArray>(operand(read.argumentsOperand()));
        return list ? settle(list->getIndex(realm, read.index())) : unknown();
    }
    case Opcode::SetMappedArgument: {
        auto store = value.as<Opcode::SetMappedArgument>();
        if (auto* list = dynamicDowncast<JSArray>(operand(store.argumentsOperand())))
            list->putDirectIndex(realm, store.index(), operand(store.valueOperand()));
        scope.clearException();
        return { };
    }

    case Opcode::ResolveHeritage: {
        JSValue heritage = operand(value.operand(0));
        if (heritage.isNull())
            return jsNull();
        if (!isKnown(heritage) || !heritage.isObject())
            return unknown();
        return settle(heritage.get(realm, vm.propertyNames->prototype));
    }
    case Opcode::NewClass: {
        auto written = value.as<Opcode::NewClass>();
        JSValue constructor = operand(written.constructorOperand());
        if (!isKnown(constructor) || !constructor.isObject())
            return unknown();
        JSValue parent = written.prototypeParentOperand() ? operand(written.prototypeParentOperand()) : JSValue(realm->objectPrototype());
        JSObject* prototype = isKnown(parent) && parent.isObject() ? constructEmptyObject(realm, asObject(parent)) : parent.isNull() ? constructEmptyObject(vm, realm->nullPrototypeObjectStructure()) : constructEmptyObject(realm);
        prototype->putDirect(vm, vm.propertyNames->constructor, constructor, static_cast<unsigned>(PropertyAttribute::DontEnum));
        asObject(constructor)->putDirect(vm, vm.propertyNames->prototype, prototype, static_cast<unsigned>(PropertyAttribute::DontEnum));
        if (JSValue heritage = operand(written.heritageOperand()); written.heritageOperand() && isKnown(heritage) && heritage.isObject())
            asObject(constructor)->setPrototype(vm, realm, heritage);
        return settle(constructor);
    }
    case Opcode::GetClassPrototype: {
        JSValue classObject = operand(value.operand(0));
        return isKnown(classObject) && classObject.isObject() ? settle(classObject.get(realm, vm.propertyNames->prototype)) : unknown();
    }
    case Opcode::DefineMethod: {
        auto definition = value.as<Opcode::DefineMethod>();
        JSValue target = operand(definition.classObjectOperand());
        if (!isKnown(target) || !target.isObject())
            return { };
        if (definition.side() == ClassSide::Prototype)
            target = target.get(realm, vm.propertyNames->prototype);
        Identifier name;
        if (definition.key())
            name = nameOf(definition.key());
        else if (JSValue computed = operand(definition.computedKeyOperand()); isKnown(computed))
            name = computed.toPropertyKey(realm);
        if (!scope.exception() && isKnown(target) && target.isObject() && !name.isNull() && definition.methodKind() != MethodKind::Setter)
            asObject(target)->putDirectMayBeIndex(realm, name, definition.methodKind() == MethodKind::Method ? operand(definition.functionOperand()) : unknown());
        scope.clearException();
        return { };
    }
    case Opcode::NewClassToken:
        return constructEmptyObject(realm);
    case Opcode::GetConstructPrototype:
        return prototypeFor(operand(value.operand(0)));
    case Opcode::GetSuperBase: {
        auto read = value.as<Opcode::GetSuperBase>();
        JSValue home = operand(read.homeOperand());
        if (isKnown(home) && home.isObject() && read.homeKind() == HomeObjectKind::ClassPrototype)
            home = home.get(realm, vm.propertyNames->prototype);
        if (scope.exception() || !isKnown(home) || !home.isObject())
            return settle({ });
        return settle(asObject(home)->getPrototype(realm));
    }
    case Opcode::SuperGet: {
        auto read = value.as<Opcode::SuperGet>();
        JSValue base = operand(read.baseOperand());
        JSValue receiver = operand(read.receiverOperand());
        Identifier name;
        if (read.key())
            name = nameOf(read.key());
        else if (JSValue computed = operand(read.computedKeyOperand()); isKnown(computed))
            name = computed.toPropertyKey(realm);
        if (scope.exception() || !isKnown(base) || !base.isObject() || name.isNull())
            return settle({ });
        PropertySlot slot(receiver, PropertySlot::InternalMethodType::Get);
        return settle(asObject(base)->getPropertySlot(realm, name, slot) ? slot.getValue(realm, name) : jsUndefined());
    }
    case Opcode::SuperConstruct: {
        auto written = value.as<Opcode::SuperConstruct>();
        return settle(construct(operand(written.parentOperand()), operand(written.newTargetOperand()), operandsOf(written.argumentsOperands()).span()));
    }
    case Opcode::SuperConstructList: {
        auto written = value.as<Opcode::SuperConstructList>();
        Values arguments;
        if (!appendElementsOf(operand(written.listOperand()), arguments))
            return unknown();
        return settle(construct(operand(written.parentOperand()), operand(written.newTargetOperand()), arguments.span()));
    }
    case Opcode::PrivateGet: {
        auto read = value.as<Opcode::PrivateGet>();
        JSValue object = operand(read.objectOperand());
        if (!isKnown(object) || !object.isObject())
            return unknown();
        return settle(asObject(object)->getDirect(vm, privateNameOf(read.layout(), static_cast<uint32_t>(read.field()))));
    }
    case Opcode::PrivateHas: {
        auto test = value.as<Opcode::PrivateHas>();
        JSValue object = operand(test.objectOperand());
        if (!isKnown(object) || !object.isObject())
            return unknown();
        return jsBoolean(!!asObject(object)->getDirect(vm, privateNameOf(test.layout(), static_cast<uint32_t>(test.field()))));
    }
    case Opcode::PrivateSet:
    case Opcode::PrivateDefine: {
        bool defines = opcode == Opcode::PrivateDefine;
        JSValue object = operand(value.operand(0));
        if (isKnown(object) && object.isObject() && object != JSValue(realm)) {
            Identifier name = defines ? privateNameOf(value.as<Opcode::PrivateDefine>().layout(), static_cast<uint32_t>(value.as<Opcode::PrivateDefine>().field())) : privateNameOf(value.as<Opcode::PrivateSet>().layout(), static_cast<uint32_t>(value.as<Opcode::PrivateSet>().field()));
            asObject(object)->putDirect(vm, name, operand(defines ? value.as<Opcode::PrivateDefine>().valueOperand() : value.as<Opcode::PrivateSet>().valueOperand()));
        }
        return { };
    }

    case Opcode::ForInOpen: {
        JSValue object = operand(value.operand(0));
        if (!isKnown(object))
            return unknown();
        JSArray* state = constructEmptyArray(realm, nullptr);
        state->push(realm, jsNumber(1));
        if (object.isObject() && object != JSValue(realm)) {
            PropertyNameArrayBuilder names(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
            asObject(object)->getPropertyNames(realm, names, DontEnumPropertiesMode::Exclude);
            if (scope.exception())
                return settle({ });
            for (const Identifier& name : names)
                state->push(realm, jsString(vm, name.string()));
        }
        return settle(state);
    }
    case Opcode::ForInStep: {
        auto* state = dynamicDowncast<JSArray>(operand(value.operand(0)));
        if (!state)
            return unknown();
        unsigned next = state->getIndex(realm, 0).toUInt32(realm);
        bool isDone = next >= state->length();
        JSValue parts[] { jsBoolean(isDone), isDone ? jsUndefined() : state->getIndex(realm, next) };
        state->putDirectIndex(realm, 0, jsNumber(next + 1));
        return settle(listOf(parts));
    }
    case Opcode::CallDirect: {
        auto written = value.as<Opcode::CallDirect>();
        return settle(enterWithEntryParameters(written.function(), operandsOf(written.argumentsOperands()).span()));
    }
    case Opcode::Results:
        return settle(listOf(operandsOf(value.as<Opcode::Results>().valuesOperands()).span()));
    case Opcode::ResultAt: {
        auto read = value.as<Opcode::ResultAt>();
        auto* results = dynamicDowncast<JSArray>(operand(read.resultsOperand()));
        return results ? settle(results->getIndex(realm, read.index())) : unknown();
    }

    case Opcode::IteratorOpen: {
        auto open = value.as<Opcode::IteratorOpen>();
        JSValue iterable = operand(open.iterableOperand());
        if (!isKnown(iterable) || open.iteratorKind() != IteratorKind::Sync)
            return unknown();
        IterationRecord record = iteratorForIterable(realm, iterable);
        if (scope.exception())
            return settle({ });
        JSValue parts[] { record.iterator, record.nextMethod };
        return settle(listOf(parts));
    }
    case Opcode::IteratorResume: {
        auto resume = value.as<Opcode::IteratorResume>();
        auto* state = dynamicDowncast<JSArray>(operand(resume.iteratorOperand()));
        if (!state || resume.resumeMode() != ResumeMode::Next)
            return unknown();
        IterationRecord record { state->getIndex(realm, 0), state->getIndex(realm, 1) };
        JSValue step = iteratorStep(realm, record);
        if (scope.exception())
            return settle({ });
        bool isDone = step.isFalse();
        JSValue parts[] { jsBoolean(isDone), isDone ? jsUndefined() : iteratorValue(realm, step) };
        if (scope.exception())
            return settle({ });
        return settle(listOf(parts));
    }

    case Opcode::EvaluateModule:
        runModule(value.as<Opcode::EvaluateModule>().module());
        return { };
    case Opcode::ModuleBuiltin: {
        auto read = value.as<Opcode::ModuleBuiltin>();
        return read.builtin() == Builtins::ModuleObject ? moduleObjectOf(read.module()) : unknown();
    }
    case Opcode::ModuleNamespace:
        return settle(namespaceOf(value.as<Opcode::ModuleNamespace>().module()));
    case Opcode::RequireBuiltinModule: {
        auto wanted = value.as<Opcode::RequireBuiltinModule>().runtimeModule();
        for (uint32_t index = 0; index < m_program.modules.size(); ++index) {
            if (m_program.modules[index].runtimeModule != wanted || !m_program.modules[index].body)
                continue;
            runModule(ModuleID(index));
            JSValue result = m_resultsOfModules[index];
            return result ? result : unknown();
        }
        return unknown();
    }
    case Opcode::RequireModule: {
        ModuleID module = value.as<Opcode::RequireModule>().module();
        if (!module || !requireAnswersWithResultOfBody(m_program, value.as<Opcode::RequireModule>().referrer(), module))
            return unknown();
        runModule(module);
        JSValue result = m_resultsOfModules[module.index()];
        return result ? result : unknown();
    }

    default:
        return unknown();
    }
}

// ---- One function

bool RehearsalOfRegExpTexts::takeEdge(Activation& activation, const BodyTerminator& terminator, uint32_t targetIndex, BodyTarget target)
{
    JSGlobalObject* realm = m_realm.get();
    BodyBlock block = activation.index.block(target.block());
    std::span<const ValueID> arguments = target.arguments();
    // Read first: an edge back to the head of a loop may pass one parameter in the place of another.
    Values passed;
    for (ValueID argument : arguments)
        passed.append(at(activation, argument));
    auto isSame = [&](JSValue held, JSValue brought) {
        return held == brought || (held.isString() && brought.isString() && asString(held)->equal(realm, asString(brought)));
    };
    bool arrivesAtJoin = activation.goesBothWays() && activation.regions.last().join == block.id();
    bool bringsOtherValues = false;
    for (uint32_t index = 0; index < passed.size(); ++index) {
        JSValue& held = activation.values[block.parameter(index).index()];
        bringsOtherValues |= !held || !isSame(held, passed[index]);
        if (!arrivesAtJoin)
            held = passed[index];
    }
    uint32_t numberOfProofs = static_cast<uint32_t>(target.proofs().size());
    for (uint32_t proofIndex = 0; proofIndex < numberOfProofs; ++proofIndex) {
        if (std::optional<BodyEdgeResult> result = activation.index.edgeResult(terminator, targetIndex, target, proofIndex); result && result->subject)
            activation.values[result->parameter.index()] = at(activation, result->subject);
    }
    if (arrivesAtJoin) {
        OpenRegion& region = activation.regions.last();
        region.hasBroughtOtherValues |= bringsOtherValues;
        bool isNew = true;
        for (uint32_t arrival = 0; arrival < region.numberOfArrivals && isNew; ++arrival) {
            isNew = false;
            for (uint32_t index = 0; index < passed.size() && !isNew; ++index)
                isNew = !isSame(activation.arrivals[region.firstArrival + arrival * passed.size() + index], passed[index]);
        }
        if (isNew) {
            ++region.numberOfArrivals;
            for (JSValue brought : passed)
                activation.arrivals.append(brought);
        }
        return false;
    }
    if (!activation.goesBothWays())
        return true;
    uint8_t& times = activation.timesEnteredWhileGoingBothWays[block.id().index()];
    if (times && (!bringsOtherValues || times >= timesThroughBlockWhileGoingBothWays))
        return false;
    if (!times++)
        activation.blocksCounted.append(block.id());
    return true;
}

BlockID RehearsalOfRegExpTexts::joinBehind(Activation& activation, BlockID block)
{
    if (activation.joins)
        return (*activation.joins)[block.index()];
    const Vector<BlockID>& joins = m_joins.ensure(activation.function.index(), [&] {
        const BodyIndex& index = activation.index;
        uint32_t numberOfBlocks = index.numberOfBlocks();
        uint32_t end = numberOfBlocks;
        constexpr uint32_t none = std::numeric_limits<uint32_t>::max();
        Vector<Vector<uint32_t, 2>> successors(numberOfBlocks + 1);
        Vector<Vector<uint32_t, 2>> predecessors(numberOfBlocks + 1);
        for (uint32_t from = 0; from < numberOfBlocks; ++from) {
            for (BodyTarget target : index.block(BlockID(from)).terminator().targets())
                successors[from].append(target.block().index());
            if (successors[from].isEmpty())
                successors[from].append(end);
            for (uint32_t to : successors[from])
                predecessors[to].append(from);
        }
        // From the end backwards, each block behind all that it leads to where that can be.
        Vector<uint32_t> order;
        Vector<uint32_t> number;
        number.fill(none, numberOfBlocks + 1);
        Vector<std::pair<uint32_t, uint32_t>> stack { { end, 0 } };
        BitVector seen(numberOfBlocks + 1);
        seen.set(end);
        while (!stack.isEmpty()) {
            auto& [node, next] = stack.last();
            if (next < predecessors[node].size()) {
                uint32_t before = predecessors[node][next++];
                if (!seen.set(before))
                    stack.append({ before, 0 });
                continue;
            }
            order.append(node);
            stack.removeLast();
        }
        order.reverse();
        for (uint32_t position = 0; position < order.size(); ++position)
            number[order[position]] = position;
        Vector<uint32_t> nearest;
        nearest.fill(none, numberOfBlocks + 1);
        nearest[end] = end;
        for (bool hasChanged = true; hasChanged;) {
            hasChanged = false;
            for (uint32_t node : order) {
                if (node == end)
                    continue;
                uint32_t found = none;
                for (uint32_t behind : successors[node]) {
                    if (nearest[behind] == none)
                        continue;
                    uint32_t other = behind;
                    if (found == none) {
                        found = other;
                        continue;
                    }
                    while (found != other) {
                        while (number[found] > number[other])
                            found = nearest[found];
                        while (number[other] > number[found])
                            other = nearest[other];
                    }
                }
                if (found != nearest[node]) {
                    nearest[node] = found;
                    hasChanged = true;
                }
            }
        }
        Vector<BlockID> joins(numberOfBlocks);
        for (uint32_t from = 0; from < numberOfBlocks; ++from) {
            if (nearest[from] != none && nearest[from] != end)
                joins[from] = BlockID(nearest[from]);
        }
        return joins;
    }).iterator->value;
    // The table may move its lists when it grows, while this function calls others.
    return joins[block.index()];
}

BlockID RehearsalOfRegExpTexts::goEachWayFrom(Activation& activation, BodyBlock block, JSValue& result)
{
    JSGlobalObject* realm = m_realm.get();
    if (!m_vm->isSafeToRecurse())
        return { };
    if (activation.timesEnteredWhileGoingBothWays.isEmpty())
        activation.timesEnteredWhileGoingBothWays.fill(0, activation.index.numberOfBlocks());
    if (uint8_t& times = activation.timesEnteredWhileGoingBothWays[block.id().index()]; !times) {
        times = 1;
        activation.blocksCounted.append(block.id());
    }
    BlockID join = joinBehind(activation, block.id());
    // `if (a) { if (b) .. }`: both meet the rest in one block, and the outer one goes on from there.
    bool sharesJoin = join && activation.goesBothWays() && activation.regions.last().join == join;
    activation.regions.append(OpenRegion { join, sharesJoin ? activation.regions.last().firstArrival : activation.arrivals.size(), sharesJoin ? activation.regions.last().numberOfArrivals : 0, false });
    ++m_openRegions;
    BodyTerminator terminator = block.terminator();
    for (uint32_t index = terminator.numberOfTargets(); index--;) {
        BodyTarget target = terminator.targets()[index];
        if (takeEdge(activation, terminator, index, target))
            runFrom(activation, target.block(), result);
    }
    OpenRegion closed = activation.regions.takeLast();
    --m_openRegions;
    if (!activation.goesBothWays()) {
        for (BlockID counted : activation.blocksCounted)
            activation.timesEnteredWhileGoingBothWays[counted.index()] = 0;
        activation.blocksCounted.shrink(0);
    }
    if (sharesJoin) {
        activation.regions.last().hasBroughtOtherValues |= closed.hasBroughtOtherValues;
        activation.regions.last().numberOfArrivals = closed.numberOfArrivals;
        return { };
    }
    Values arrivals;
    for (size_t arrival = closed.firstArrival; arrival < activation.arrivals.size(); ++arrival)
        arrivals.append(activation.arrivals[arrival]);
    while (activation.arrivals.size() > closed.firstArrival)
        activation.arrivals.removeLast();
    if (!closed.numberOfArrivals)
        return { };
    // The head of a loop around the branch, inside a region that is still open.
    if (activation.goesBothWays()) {
        uint8_t& times = activation.timesEnteredWhileGoingBothWays[join.index()];
        if (times && (!closed.hasBroughtOtherValues || times >= timesThroughBlockWhileGoingBothWays))
            return { };
        if (!times++)
            activation.blocksCounted.append(join);
    }

    BodyBlock joined = activation.index.block(join);
    // Behind these the block may have parameters for what its edges prove.
    uint32_t numberBrought = static_cast<uint32_t>(arrivals.size() / closed.numberOfArrivals);
    // What nobody knows on one way gives way to what another way knows: the list claims nothing, and a guess that is wrong costs a miss.
    Values agreed;
    bool differ = false;
    for (uint32_t index = 0; index < numberBrought; ++index) {
        JSValue known;
        bool differsHere = false;
        for (uint32_t arrival = 0; arrival < closed.numberOfArrivals; ++arrival) {
            JSValue brought = arrivals[arrival * numberBrought + index];
            if (!isKnown(brought))
                continue;
            if (!known)
                known = brought;
            else if (known != brought && !(known.isString() && brought.isString() && asString(known)->equal(realm, asString(brought))))
                differsHere = true;
        }
        agreed.append(known && !differsHere ? known : unknown());
        differ |= differsHere;
    }
    auto bring = [&](uint32_t arrival) {
        for (uint32_t index = 0; index < numberBrought; ++index) {
            JSValue brought = arrivals[arrival * numberBrought + index];
            activation.values[joined.parameter(index).index()] = isKnown(brought) ? brought : agreed[index];
        }
    };
    // `new RegExp(text, sticky ? flags + "y" : flags)`: what stands behind is gone through once for each of the values. In a loop that would be once for each of the ways through all its turns.
    if (differ && !activation.index.loopOf(join) && activation.numberOfWaysGoneSeparately + closed.numberOfArrivals <= timesThroughBlockWhileGoingBothWays) {
        activation.numberOfWaysGoneSeparately += closed.numberOfArrivals - 1;
        bring(0);
        for (uint32_t arrival = 1; arrival < closed.numberOfArrivals; ++arrival) {
            runOneOfWays(activation, join, result, closed.numberOfArrivals - arrival + 1);
            bring(arrival);
        }
        return join;
    }
    for (uint32_t index = 0; index < numberBrought; ++index)
        activation.values[joined.parameter(index).index()] = agreed[index];
    return join;
}

// Each goes to the end of the function. None uses up what the ways behind it need.
void RehearsalOfRegExpTexts::runOneOfWays(Activation& activation, BlockID block, JSValue& result, uint32_t numberOfWaysLeft)
{
    uint32_t kept = m_operationsLeft - m_operationsLeft / numberOfWaysLeft;
    m_operationsLeft -= kept;
    runFrom(activation, block, result);
    m_operationsLeft = static_cast<uint32_t>(std::min<uint64_t>(static_cast<uint64_t>(m_operationsLeft) + kept, std::numeric_limits<uint32_t>::max()));
}

void RehearsalOfRegExpTexts::runFrom(Activation& activation, BlockID current, JSValue& result)
{
    JSGlobalObject* realm = m_realm.get();
    const BodyIndex& index = activation.index;
    // As where ways meet again: what one way knows stands, unless another knows otherwise.
    auto answer = [&](JSValue returned) {
        if (!result || !isKnown(result))
            result = activation.waysDifferInWhatTheyReturn ? unknown() : returned;
        else if (isKnown(returned) && !JSValue::strictEqual(realm, result, returned)) {
            activation.waysDifferInWhatTheyReturn = true;
            result = unknown();
        }
    };

    while (current) {
        BodyBlock block = index.block(current);
        if (!hasRoomLeft())
            end();
        for (BodyValue value : block.values()) {
            if (!m_operationsLeft || !m_operationsLeftInAll) {
                result = unknown();
                return;
            }
            --m_operationsLeft;
            --m_operationsLeftInAll;
            activation.values[value.id().index()] = execute(activation, value);
        }

        BodyTerminator terminator = block.terminator();
        current = { };
        switch (terminator.kind()) {
        case TerminatorKind::Jump:
            if (takeEdge(activation, terminator, 0, terminator.next()))
                current = terminator.next().block();
            break;
        case TerminatorKind::Branch: {
            JSValue condition = at(activation, terminator.operand(0));
            if (!isKnown(condition)) {
                current = goEachWayFrom(activation, block, result);
                break;
            }
            bool isTaken = condition.toBoolean(realm);
            BodyTarget target = isTaken ? terminator.taken() : terminator.notTaken();
            if (takeEdge(activation, terminator, isTaken ? 0 : 1, target))
                current = target.block();
            break;
        }
        case TerminatorKind::Switch:
        case TerminatorKind::SwitchString: {
            JSValue subject = at(activation, terminator.operand(0));
            if (!isKnown(subject)) {
                current = goEachWayFrom(activation, block, result);
                break;
            }
            uint32_t chosen = terminator.numberOfTargets() - 1;
            uint32_t position = 0;
            if (terminator.kind() == TerminatorKind::Switch && subject.isNumber()) {
                for (int64_t written : terminator.as<TerminatorKind::Switch>().cases()) {
                    if (static_cast<double>(written) == subject.asNumber()) {
                        chosen = position;
                        break;
                    }
                    ++position;
                }
            } else if (terminator.kind() == TerminatorKind::SwitchString && subject.isString()) {
                String text = asString(subject)->value(realm);
                for (StringID written : terminator.as<TerminatorKind::SwitchString>().strings()) {
                    if (charactersOf(written) == text) {
                        chosen = position;
                        break;
                    }
                    ++position;
                }
            }
            BodyTarget target = terminator.targets()[chosen];
            if (takeEdge(activation, terminator, chosen, target))
                current = target.block();
            break;
        }
        case TerminatorKind::Return:
        case TerminatorKind::ReturnCall:
            answer(at(activation, terminator.operand(0)));
            break;
        default:
            break;
        }
    }
}

JSValue RehearsalOfRegExpTexts::runBlocks(Activation& activation)
{
    // Nothing: no way has come to an end.
    JSValue result;
    runFrom(activation, entryBlock, result);
    return result ? result : unknown();
}

JSValue RehearsalOfRegExpTexts::enterWithEntryParameters(FunctionID function, std::span<const JSValue> entryParameters)
{
    const BodyIndex* index = m_bodies.find(function);
    if (!index || m_depth >= deepestCall || !m_operationsLeft || !m_operationsLeftInAll || !m_vm->isSafeToRecurse())
        return unknown();
    uint32_t price = index->numberOfValues() / valuesMadeRoomForAtPriceOfOneOperation;
    if (price >= m_operationsLeft || price >= m_operationsLeftInAll)
        return unknown();
    // A function that calls itself behind a branch that nobody knows: nobody knows where that ends.
    unsigned& regionsOpenAtEntryPlusOne = m_regionsOpenAtEntryPlusOne[function.index()];
    if (regionsOpenAtEntryPlusOne && regionsOpenAtEntryPlusOne <= m_openRegions)
        return unknown();
    SetForScope entered(regionsOpenAtEntryPlusOne, m_openRegions + 1);
    m_operationsLeft -= price;
    m_operationsLeftInAll -= price;
    SetForScope deeper(m_depth, m_depth + 1);
    Activation activation { *index, function, { }, { }, { } };
    activation.values.fill(*m_vm, index->numberOfValues(), [](JSValue*) { });
    BodyBlock entry = index->block(entryBlock);
    for (uint32_t parameter = 0; parameter < entry.numberOfParameters(); ++parameter)
        activation.values[entry.parameter(parameter).index()] = parameter < entryParameters.size() ? entryParameters[parameter] : unknown();
    JSValue result = runBlocks(activation);
    const Function& record = m_program.at(function);
    if (!record.has(FunctionFlags::IsAsync) || record.has(FunctionFlags::IsGenerator) || !isKnown(result))
        return result;
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(*m_vm);
    JSValue promise = JSPromise::resolvedPromise(m_realm.get(), result);
    if (scope.exception()) {
        scope.clearException();
        return unknown();
    }
    return promise;
}

JSValue RehearsalOfRegExpTexts::enterWithNothingKnown(FunctionID function)
{
    return enterWithEntryParameters(function, { });
}

JSValue RehearsalOfRegExpTexts::enter(FunctionID function, std::span<const JSValue> captures, JSValue thisValue, JSValue callee, JSValue newTarget, std::span<const JSValue> arguments)
{
    const Function& record = m_program.at(function);
    Values entryParameters;
    unsigned numberOfEntryParameters = record.numberOfEntryParameters();
    unsigned positional = record.numberOfPositionalParameters();
    auto behind = [&](unsigned first) {
        return listOf(arguments.subspan(std::min<size_t>(first, arguments.size())));
    };
    unsigned nextCapture = 0;
    unsigned nextParameter = 0;
    for (unsigned index = 0; index < numberOfEntryParameters; ++index) {
        switch (record.entryParameterKind(index)) {
        case EntryParameter::Capture:
            entryParameters.append(nextCapture < captures.size() ? captures[nextCapture] : unknown());
            ++nextCapture;
            break;
        case EntryParameter::This:
            entryParameters.append(thisValue);
            break;
        case EntryParameter::Callee:
            entryParameters.append(callee);
            break;
        case EntryParameter::NewTarget:
            entryParameters.append(newTarget);
            break;
        case EntryParameter::ArgumentCount:
            entryParameters.append(jsNumber(static_cast<uint32_t>(arguments.size())));
            break;
        case EntryParameter::Parameter:
            if (nextParameter < positional)
                entryParameters.append(nextParameter < arguments.size() ? arguments[nextParameter] : jsUndefined());
            else
                entryParameters.append(behind(positional));
            ++nextParameter;
            break;
        case EntryParameter::HiddenRest:
            entryParameters.append(behind(positional));
            break;
        }
    }
    return enterWithEntryParameters(function, entryParameters.span());
}

void RehearsalOfRegExpTexts::runModule(ModuleID id)
{
    StateOfModule& state = m_statesOfModules[id.index()];
    FunctionID body = m_program.at(id).body;
    // Too deep, or nothing left of the allowance: it is entered from a start of its own, or the next time that somebody asks.
    if (state != StateOfModule::IsNotEntered || !body || m_depth >= deepestCall || !m_operationsLeft || !m_operationsLeftInAll || !m_vm->isSafeToRecurse())
        return;
    state = StateOfModule::IsRunning;
    m_rehearsed.set(body.index());
    // `import "./routes"`: the loader has run what the module imports, in the order in which that is written. Elsewhere than in the start of the program, and of the library, a module is run when somebody asks for a variable of it.
    if (m_goesThroughStartOfProgram) {
        for (const ModuleRequest& request : m_program.at(id).requests) {
            if (request.kind == ModuleRequestKind::Import && request.phase == ImportPhase::Evaluation && !m_program.at(request.target).flags.contains(ModuleFlags::IsLibrary))
                runModule(request.target);
        }
    }
    // `exports.name = ..`: the loader has made the object.
    if (VariableID exported = m_program.at(id).exportsVariable; exported && !m_variables[exported.index()])
        m_variables[exported.index()] = constructEmptyObject(m_realm.get());
    m_resultsOfModules[id.index()] = enterWithNothingKnown(body);
    state = StateOfModule::HasRun;
}

// ---- The program

bool RehearsalOfRegExpTexts::rehearse(FunctionID function)
{
    const Function& record = m_program.at(function);
    m_operationsLeft = operationsFromOneStart;
    m_operationsLeftInAllAtLastText = m_operationsLeftInAll;
    m_sawTextThatNobodyKnows = false;
    if (record.isModuleBody())
        runModule(record.module);
    else if (!m_rehearsed.set(function.index()))
        enterWithNothingKnown(function);
    return m_sawTextThatNobodyKnows;
}

void RehearsalOfRegExpTexts::run(std::span<const FunctionID> functionsThatCallRegExp)
{
    if (!isKnown(m_regExpConstructor))
        return;
    uint64_t operationsInAll = m_operationsLeftInAll;

    // `app.get("/users/:id", ..)`: the text comes many calls down from the body of the module that the program starts with. That body is gone through as the program goes through it.
    for (const Entry& entry : m_program.entries) {
        m_operationsLeft = static_cast<uint32_t>(std::min<uint64_t>(m_operationsLeftInAll, std::numeric_limits<uint32_t>::max()));
        m_operationsLeftInAllAtLastText = m_operationsLeftInAll;
        SetForScope goesThroughStartOfProgram(m_goesThroughStartOfProgram, true);
        runModule(entry.module);
    }

    // A router that makes its one pattern at the first request, out of the routes that the start of the program has given it: each function that was handed to code that nobody knows is called once, with what the start has left behind
    // and nothing known of what it is handed.
    for (size_t index = 0; index < m_handlers.size(); ++index) {
        ClosureOfProgram handler = closureIn(m_handlers[index]);
        m_operationsLeft = operationsFromOneStart;
        m_operationsLeftInAllAtLastText = m_operationsLeftInAll;
        Values captures;
        appendElementsOf(handler.captures, captures);
        Values arguments;
        for (unsigned parameter = m_program.at(handler.function).numberOfPositionalParameters(); parameter--;)
            arguments.append(unknown());
        enter(handler.function, captures.span(), unknown(), m_handlers[index], jsUndefined(), arguments.span());
    }

    // A start of the program that never ends takes nothing from the starts that follow.
    if (m_isOver)
        return;
    m_operationsLeftInAll = operationsInAll;

    size_t numberOfFunctions = m_program.functions.size();
    BitVector lacksTexts(numberOfFunctions);
    for (FunctionID function : functionsThatCallRegExp) {
        if (rehearse(function))
            lacksTexts.set(function.index());
    }

    // `function matcher(word) { return new RegExp("^" + word) }`, `rules.map(rule => new RegExp(rule.source))`: whoever names the function may call it, or hand it to what calls it, with what the rehearsal knows.
    for (size_t index = 0; index < numberOfFunctions && !lacksTexts.isEmpty(); ++index) {
        FunctionID function(static_cast<uint32_t>(index));
        if (m_rehearsed.get(index))
            continue;
        bool namesOne = std::ranges::any_of(m_summaries.at(function).functionsNamed, [&](FunctionID named) {
            return lacksTexts.get(named.index());
        });
        if (namesOne)
            rehearse(function);
    }
}

void RehearsalOfRegExpTexts::storeTexts(ProgramFactTables& program)
{
    struct TextWithBytes {
        Vector<uint8_t> bytes;
        RegExpTextWorkedOut text;
    };

    Vector<TextWithBytes> textsWorkedOut;
    for (RegExpTextWorkedOut& text : m_found) {
        Vector<uint8_t> bytes = encodeProgramString(StringView(text.pattern));
        if (StringID spelled = m_program.strings.find(bytes.span()))
            program.regExpTextsInSight.append(RegExpTextInSight { spelled, text.flags, text.isOneOfAllAtSomeCall });
        else
            textsWorkedOut.append(TextWithBytes { WTF::move(bytes), WTF::move(text) });
    }
    m_found.clear();

    AOT::sort(program.regExpTextsInSight, { }, [](const RegExpTextInSight& text) {
        return std::tuple(text.pattern, text.flags);
    });
    AOT::sort(textsWorkedOut, [](const TextWithBytes& a, const TextWithBytes& b) {
        std::strong_ordering order = compareProgramStrings(a.bytes.span(), b.bytes.span());
        return order != std::strong_ordering::equal ? order == std::strong_ordering::less : a.text.flags < b.text.flags;
    });
    program.regExpTextsWorkedOut.reserveInitialCapacity(textsWorkedOut.size());
    for (TextWithBytes& text : textsWorkedOut)
        program.regExpTextsWorkedOut.append(WTF::move(text.text));
}

// Ascending. `new RegExp("a+", "g")`: of texts that are written the walk makes the literal, and nobody need work them out.
// Code of the library is no start. It comes with every program, so every build would make a realm for it, and what it hands RegExp by itself is written, or is what the process finds when it runs:
// a variable of the environment, the value to print. It is gone through where the program calls it.
static Vector<FunctionID> functionsThatCallRegExpWithTextsToWorkOut(const Program& program, const BodyIndexTable& bodies, const ThinSummaries& summaries)
{
    const BuiltinTable& table = program.builtins();
    BuiltinID regExp = table.canonicalID(Builtins::RegExp);
    Vector<FunctionID> functions;
    for (size_t number = 0; number < program.functions.size(); ++number) {
        FunctionID function(static_cast<uint32_t>(number));
        const BodyIndex* index = bodies.find(function);
        if (!index || isLibraryCode(program, program.at(function)))
            continue;
        auto isWrittenAs = [&](ValueID operand, Opcode opcode) {
            ValueID behindCasts = index->valueBehindCasts(operand);
            return !index->isParameter(behindCasts) && index->opcodeOf(behindCasts) == opcode;
        };
        bool hasTextsToWorkOut = std::ranges::any_of(summaries.at(function).calls, [&](const SummaryCall& call) {
            if (call.callee != SummaryCall::Callee::Builtin || table.canonicalID(call.builtin) != regExp)
                return false;
            BodyValue written = index->value(call.site);
            if (written.opcode() != Opcode::Call && written.opcode() != Opcode::Construct)
                return true;
            std::span<const ValueID> arguments = written.variadicOperands();
            if (arguments.empty() || !isWrittenAs(arguments[0], Opcode::String))
                return true;
            return arguments.size() > 1 && !isWrittenAs(arguments[1], Opcode::String) && !isWrittenAs(arguments[1], Opcode::Undefined);
        });
        if (hasTextsToWorkOut)
            functions.append(function);
    }
    return functions;
}

void workOutRegExpTexts(const Program& program, const BodyIndexTable& bodies, const ThinSummaries& summaries, ProgramFactTables& tables)
{
    // Many built-in functions of a realm are bytecode.
    if (Options::crashIfInterpreterIsUsed())
        return;
    Vector<FunctionID> functions = functionsThatCallRegExpWithTextsToWorkOut(program, bodies, summaries);
    if (functions.isEmpty())
        return;
    // The one thread does it all: a build starts the threads that it counts, and says so where it is refused one.
    RuntimeHooks::NoHelperThreadsScope noHelperThreads;
    RefPtr<VM> vm = VM::create(HeapType::Small);
    Collector& collector = vm->heap.collector();
    collector.willCollectWithoutHelpers();
    {
        JSLockHolder locker(*vm);
        {
            RehearsalOfRegExpTexts rehearsal(*vm, program, bodies, summaries);
            rehearsal.run(functions.span());
            rehearsal.storeTexts(tables);
        }
        // The lock holder is left with the last reference, so the VM dies under its lock.
        vm = nullptr;
    }
    // A collector of its own has died with the VM.
    if (Options::useGlobalGC())
        collector.didCollectWithoutHelpers();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
