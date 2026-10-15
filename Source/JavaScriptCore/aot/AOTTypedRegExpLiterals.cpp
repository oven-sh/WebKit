/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedPasses.h"

#if ENABLE(AOT)

#include "AOTImage.h"
#include "AOTTypedBuiltins.h"
#include "AOTTypedData.h"
#include "AOTTypedFormat.h"
#include "DeferGC.h"
#include "GetterSetter.h"
#include "JSCInlines.h"
#include "JSMap.h"
#include "JSNativeStdFunction.h"
#include "JSONObject.h"
#include "JSSet.h"
#include "MathCommon.h"
#include "ObjectConstructor.h"
#include "RegExpConstructor.h"
#include "RegExpInlines.h"
#include "RegExpObject.h"
#include "TopExceptionScope.h"
#include "YarrFlags.h"
#include <wtf/Box.h>
#include <wtf/Scope.h>
#include <wtf/SetForScope.h>
#include <wtf/StringPrintStream.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr size_t maxConstantTextSize = MB;

static bool appendConstantText(const Program& program, const Function& function, ValueID id, Vector<uint8_t>& text)
{
    auto& value = function.at(id);
    switch (value.opcode) {
    case Opcode::String:
        text.append(program.string(value.string()));
        return text.size() <= maxConstantTextSize;
    case Opcode::Cast:
    case Opcode::ToString:
        return appendConstantText(program, function, value.child(0), text);
    case Opcode::Add:
        return appendConstantText(program, function, value.child(0), text) && appendConstantText(program, function, value.child(1), text);
    // Where the text does not say hot a Strcat is a call of a function that is nothing but that: see giveConcatenationsOneBody().
    case Opcode::CallDirect: {
        auto& callee = program.at(value.function());
        if (callee.blocks.size() != 1 || callee.blocks[0].values.size() != 1 || callee.blocks[0].terminator.kind != TerminatorKind::Return || callee.blocks[0].terminator.value != callee.blocks[0].values[0])
            return false;
        auto& body = callee.at(callee.blocks[0].values[0]);
        if (body.opcode != Opcode::Strcat || body.arguments.size() != value.arguments.size() || !std::ranges::equal(body.arguments, callee.blocks[0].parameters))
            return false;
        [[fallthrough]];
    }
    case Opcode::Strcat:
        for (ValueID argument : value.arguments) {
            if (!appendConstantText(program, function, argument, text))
                return false;
        }
        return true;
    default:
        return false;
    }
}

using PossibleFlags = Vector<OptionSet<Yarr::Flags>, 4>;

// `onlyFirst ? undefined : "g"`: whatever constant an edge passes. Where nothing is known: none, which is a guess.
static void appendPossibleFlags(const Program& program, const Function& function, ValueID id, PossibleFlags& all, unsigned joinsLeft)
{
    auto& value = function.at(id);
    Vector<uint8_t> text;
    if (value.opcode == Opcode::Undefined || appendConstantText(program, function, id, text)) {
        if (auto flags = Yarr::parseFlags(StringView { byteCast<Latin1Character>(text.span()) }); flags && !all.contains(*flags))
            all.append(*flags);
        return;
    }
    if (value.opcode == Opcode::Cast)
        return appendPossibleFlags(program, function, value.child(0), all, joinsLeft);
    if (value.opcode == Opcode::Parameter && joinsLeft) {
        for (unsigned joined = 1; joined < function.blocks.size(); ++joined) {
            size_t index = function.blocks[joined].parameters.find(id);
            if (index == notFound)
                continue;
            for (auto& block : function.blocks) {
                for (auto& target : block.terminator.targets) {
                    if (target.block.index() == joined && index < target.arguments.size())
                        appendPossibleFlags(program, function, target.arguments[index], all, joinsLeft - 1);
                }
            }
            return;
        }
    }
    if (!all.contains(OptionSet<Yarr::Flags> { }))
        all.append(OptionSet<Yarr::Flags> { });
}

// What the code of the modules makes of the texts that are written down in it, as far as the program alone says. marked builds its 42 expressions when it is loaded:
// `edit(/^(?:bull)/).replace(/bull/g, bullet).getRegex()`. This goes through that code with what it knows: primitives, expressions, and the closures, objects and arrays the code
// itself has made; everything else is unknown, and so is what is made of it. Whatever RegExp comes about with a known source and known flags is handed to the image.
// It proves nothing and changes nothing: see addRegExpLiterals(). So where it does not know which way a branch goes it guesses, it takes a call that throws for one that
// yields something unknown, and it stops when it has done enough. It goes by the program alone: the same program, the same image.
class EvaluatorOfTexts {
public:
    EvaluatorOfTexts(VM& vm, const Program& program, ImageBuilder& builder)
        : m_vm(vm)
        , m_program(program)
        , m_builder(builder)
    {
    }

    static String decode(std::span<const uint8_t> bytes)
    {
        if (String text = String::fromUTF8(bytes); !text.isNull())
            return text;
        return String(decodeTextOfProgram(bytes).span());
    }

    void run()
    {
        if (m_program.buffer.size() < sizeof(Format::Header))
            return;
        auto& header = *reinterpret_cast<const Format::Header*>(m_program.buffer.data());
        m_words = section<uint32_t>(header.words);
        m_bytes = section<uint8_t>(header.bytes);
        m_strings = section<Format::String>(header.strings);
        m_shapes = section<Format::Shape>(header.shapes);
        m_fields = section<Format::Field>(header.fields);
        m_methods = section<Format::Method>(header.methods);
        m_variablesOfProgram = section<Format::Variable>(header.variables);
        m_modules = section<Format::Module>(header.modules);
        m_functions = section<Format::Function>(header.functions);
        m_blocks = section<Format::Block>(header.blocks);
        m_values = section<Format::Value>(header.values);
        m_targets = section<Format::Target>(header.targets);
        findDistancesToSites();
        if (!m_hasSites)
            return;

        DeferGC deferGC(m_vm);
        auto die = makeScopeExit([&] {
            *m_isAlive = false;
        });
        prepareGlobalObject();
        auto declaredTypes = section<Format::DeclaredType>(header.declaredTypes);
        m_variables.grow(m_variablesOfProgram.size());
        m_isStored.ensureSize(m_variablesOfProgram.size());
        for (unsigned i = 0; i < m_variables.size(); ++i) {
            auto& variable = m_variablesOfProgram[i];
            if (variable.initialization == Format::Initialization::Undefined) {
                m_variables[i] = jsUndefined();
                m_isStored.quickSet(i);
            } else if (variable.initialization == Format::Initialization::Zero && variable.declaredType < declaredTypes.size()) {
                m_variables[i] = declaredTypes[variable.declaredType].primitives & Format::PrimitiveNumber ? jsNumber(0) : jsBoolean(false);
                m_isStored.quickSet(i);
            }
        }
        for (uint32_t i = 0; i < m_shapes.size(); ++i) {
            if (m_shapes[i].kind == Format::ShapeKind::Instance && m_shapes[i].constructor < m_functions.size())
                m_shapeOfConstructor.add(m_shapes[i].constructor, i);
        }

        struct Pass {
            unsigned walks { 0 };
            unsigned walksCutShort { 0 };
            uint64_t steps { 0 };
            uint64_t stepsOfWalksCutShort { 0 };
            unsigned handedOver { 0 };
            unsigned handedOverWhereCutShort { 0 };
            Seconds time;
        };
        std::array<Pass, 2> passes;
        unsigned pass = 0;
        auto counted = [&](unsigned given, const auto& walk) {
            if (!m_remarks) [[likely]] {
                walk();
                return;
            }
            unsigned handedOverBefore = m_numberOfExpressions + m_numberOfExpressionsWithoutCode;
            MonotonicTime from = MonotonicTime::now();
            walk();
            auto& current = passes[pass];
            unsigned spent = given - m_stepsLeft - m_stepsInReserve;
            unsigned handedOver = m_numberOfExpressions + m_numberOfExpressionsWithoutCode - handedOverBefore;
            current.time += MonotonicTime::now() - from;
            ++current.walks;
            current.steps += spent;
            current.handedOver += handedOver;
            if (!m_stepsLeft) {
                ++current.walksCutShort;
                current.stepsOfWalksCutShort += spent;
                current.handedOverWhereCutShort += handedOver;
            }
        };
        constexpr unsigned stepsNearSites = maxNumberOfSteps * 4;
        constexpr unsigned stepsOfWalk = maxNumberOfSteps / 64;
        Vector<uint32_t> rootsOfLongTexts;
        unsigned stepsLeftNearSites = stepsNearSites;
        auto enter = [&](uint32_t index, unsigned atMost) {
            unsigned reserved = std::min(stepsLeftNearSites, atMost);
            m_stepsLeft = m_functions[index].kind == Format::FunctionKind::Module ? reserved : std::min(reserved, numberOfStepsBeforeAnyText);
            m_stepsInReserve = reserved - m_stepsLeft;
            m_hasMadeLongText = false;
            auto blocks = slice(m_blocks, m_functions[index].blocks);
            counted(reserved, [&] {
                invoke(index, Vector<JSValue>(blocks.empty() ? 0 : blocks[0].parameters.count));
            });
            remarkOnWalk(index);
            stepsLeftNearSites -= reserved - m_stepsLeft - m_stepsInReserve;
            m_stepsInReserve = 0;
        };
        for (unsigned distance = 0; distance <= maxDistanceToSite; ++distance) {
            for (uint32_t i = 0; i < m_functions.size() && stepsLeftNearSites; ++i) {
                if (m_distanceToSite[i] != distance || m_wasEntered.get(i))
                    continue;
                enter(i, m_functions[i].kind == Format::FunctionKind::Module ? maxNumberOfSteps / 8 : stepsOfWalk);
                if (!m_stepsLeft && m_hasMadeLongText)
                    rootsOfLongTexts.append(i);
            }
        }
        std::array<unsigned, maxDistanceToSite + 1> neverEntered { };
        if (m_remarks) [[unlikely]] {
            for (uint32_t i = 0; i < m_functions.size(); ++i) {
                if (m_distanceToSite[i] <= maxDistanceToSite && !m_wasEntered.get(i) && m_functions[i].blocks.count)
                    ++neverEntered[m_distanceToSite[i]];
            }
        }
        stepsLeftNearSites = maxNumberOfSteps * 6;
        pass = 1;
        for (uint32_t index : rootsOfLongTexts) {
            if (!stepsLeftNearSites)
                break;
            enter(index, maxNumberOfSteps / 3);
        }
        if (m_remarks) [[unlikely]] {
            static constexpr ASCIILiteral names[] = { "near the sites"_s, "the roots of long texts, again"_s };
            const unsigned budgets[] = { stepsNearSites, maxNumberOfSteps * 6 };
            for (unsigned k = 0; k < 2; ++k)
                dataLogLn("AOT: evaluator of texts: ", names[k], ": ", passes[k].walks, " walks, ", passes[k].steps, " steps of ", budgets[k], ", ", static_cast<unsigned>(passes[k].time.milliseconds()), " ms, ", passes[k].handedOver, " expressions handed over; cut short with no steps left: ", passes[k].walksCutShort, " walks, ", passes[k].stepsOfWalksCutShort, " steps, ", passes[k].handedOverWhereCutShort, " expressions handed over");
            dataLogLn("AOT: evaluator of texts: a walk from a function has at most ", stepsOfWalk, " steps; functions near a site that the first pass never entered, at distance 0 | 1 | 2 | 3: ", neverEntered[0], " | ", neverEntered[1], " | ", neverEntered[2], " | ", neverEntered[3], "; modules run for a variable: ", maxNumberOfSteps * 8 - m_stepsLeftForModules, " steps of ", maxNumberOfSteps * 8);
        }
        remarkOnSites();
    }

private:
    static constexpr unsigned maxDistanceToSite = 3;
    static constexpr unsigned minLengthOfLongText = 1024;
    static constexpr unsigned maxNumberOfSteps = 3000000;
    static constexpr unsigned numberOfStepsBeforeAnyText = 8192;
    static constexpr unsigned maxDepth = 48;
    static constexpr unsigned maxNumberOfClosures = 200000;
    static constexpr unsigned maxNumberOfGuesses = 256;
    static constexpr unsigned maxNumberOfChoices = 8;
    static constexpr unsigned maxNumberOfExpressions = 1024;
    static constexpr unsigned maxNumberOfExpressionsWithoutCode = 32;
    static constexpr unsigned maxSizeToGoBothWays = 256;
    static constexpr unsigned maxNumberOfBranchesGoneBothWays = 3;
    static constexpr double maxNumberHandedToBuiltin = 1 << 24;
    static constexpr unsigned maxNumberOfObjectsSized = 4096;
    static constexpr unsigned costOfWhatIsTooLargeToSay = std::numeric_limits<unsigned>::max();

    struct Note {
        uint32_t origin { 0 };
        uint32_t length { 0 };
    };

    struct Frame {
        uint32_t function;
        std::span<const Format::Value> values;
        std::span<const Format::Block> blocks;
        Vector<JSValue> slots;
        Vector<Note> notes;
    };

    struct Closure {
        uint32_t function;
        Vector<JSValue> captures;
    };

    struct Iteration {
        JSValue iterator;
        JSValue next;
        JSValue current;
    };

    struct Origin {
        uint32_t function;
        uint32_t value;
        ASCIILiteral operand;
        String name;
    };

    template<typename T>
    std::span<const T> section(Format::Section section) const
    {
        auto buffer = m_program.buffer;
        if (section.offsetInBytes % alignof(T) || static_cast<uint64_t>(section.offsetInBytes) + static_cast<uint64_t>(section.count) * sizeof(T) > buffer.size())
            return { };
        return { reinterpret_cast<const T*>(buffer.data() + section.offsetInBytes), section.count };
    }

    template<typename T>
    static std::span<const T> slice(std::span<const T> all, Format::Span span)
    {
        if (static_cast<uint64_t>(span.start) + span.count > all.size())
            return { };
        return all.subspan(span.start, span.count);
    }

    std::span<const uint8_t> bytesOf(uint32_t string) const
    {
        if (string >= m_strings.size())
            return { };
        return slice(m_bytes, Format::Span { m_strings[string].offset, m_strings[string].length });
    }

    bool isNamed(uint32_t string, ASCIILiteral name) const { return equalSpans(bytesOf(string), name.span8()); }

    JSString* textOf(uint32_t string)
    {
        if (string >= m_strings.size())
            return nullptr;
        if (m_texts.isEmpty())
            m_texts.fill(nullptr, m_strings.size());
        if (!m_texts[string]) {
            String text = decode(bytesOf(string));
            if (text.isNull())
                return nullptr;
            m_texts[string] = jsString(m_vm, WTF::move(text));
        }
        return m_texts[string];
    }

    const Identifier& identifierOf(uint32_t string)
    {
        if (m_identifiers.isEmpty())
            m_identifiers.grow(m_strings.size() + 1);
        if (string >= m_strings.size())
            return m_identifiers.last();
        if (m_identifiers[string].isNull()) {
            String text = decode(bytesOf(string));
            if (!text.isNull())
                m_identifiers[string] = Identifier::fromString(m_vm, text);
        }
        return m_identifiers[string];
    }

    Identifier nameOfField(const Format::Value& value)
    {
        if (value.immediates[0] >= m_shapes.size())
            return { };
        auto fields = slice(m_fields, m_shapes[value.immediates[0]].fields);
        if (value.immediates[1] >= fields.size())
            return { };
        if (fields[value.immediates[1]].name != Format::none)
            return identifierOf(fields[value.immediates[1]].name);
        return Identifier::fromString(m_vm, makeString('\0', value.immediates[0], '.', value.immediates[1]));
    }

    bool isSite(std::span<const Format::Value> values, const Format::Value& value) const
    {
        if (value.opcode != Format::Opcode::Construct && value.opcode != Format::Opcode::Call)
            return false;
        if (value.children[0] >= values.size() || values[value.children[0]].opcode != Format::Opcode::GetGlobal || !isNamed(values[value.children[0]].immediates[0], "RegExp"_s))
            return false;
        for (uint32_t argument : slice(m_words, value.arguments)) {
            if (argument >= values.size() || values[argument].opcode != Format::Opcode::String)
                return true;
        }
        return false;
    }

    void findDistancesToSites()
    {
        unsigned numberOfFunctions = m_functions.size();
        m_distanceToSite.fill(std::numeric_limits<uint8_t>::max(), numberOfFunctions);
        Vector<Vector<uint32_t>> namedBy(numberOfFunctions);
        Vector<uint32_t> level;
        for (uint32_t index = 0; index < numberOfFunctions; ++index) {
            auto values = slice(m_values, m_functions[index].values);
            for (auto& value : values) {
                switch (value.opcode) {
                case Format::Opcode::Construct:
                case Format::Opcode::Call:
                    if (m_distanceToSite[index] && isSite(values, value)) {
                        m_distanceToSite[index] = 0;
                        level.append(index);
                    }
                    break;
                case Format::Opcode::FunctionReference:
                case Format::Opcode::NewClosure:
                case Format::Opcode::NewClosureOnce:
                case Format::Opcode::CallDirect:
                    if (value.immediates[0] < numberOfFunctions)
                        namedBy[value.immediates[0]].append(index);
                    break;
                default:
                    break;
                }
            }
        }
        m_hasSites = !level.isEmpty();
        for (auto& shape : m_shapes) {
            if (shape.constructor >= numberOfFunctions)
                continue;
            for (auto& method : slice(m_methods, shape.methods)) {
                if (method.function < numberOfFunctions)
                    namedBy[method.function].append(shape.constructor);
            }
        }
        for (unsigned distance = 1; distance <= maxDistanceToSite; ++distance) {
            Vector<uint32_t> next;
            for (uint32_t index : level) {
                for (uint32_t other : namedBy[index]) {
                    if (m_distanceToSite[other] <= distance)
                        continue;
                    m_distanceToSite[other] = distance;
                    next.append(other);
                }
            }
            level = WTF::move(next);
        }
    }

    const BitVector& blocksLeadingToSites(const Frame& frame)
    {
        return m_blocksLeadingToSites.ensure(frame.function, [&] {
            BitVector leads;
            leads.ensureSize(frame.blocks.size());
            unsigned distance = m_distanceToSite[frame.function];
            if (distance > maxDistanceToSite)
                return leads;
            for (unsigned i = 0; i < frame.blocks.size(); ++i) {
                for (uint32_t id : slice(m_words, frame.blocks[i].values)) {
                    if (id >= frame.values.size())
                        continue;
                    auto& value = frame.values[id];
                    bool names = value.opcode == Format::Opcode::CallDirect || value.opcode == Format::Opcode::NewClosure || value.opcode == Format::Opcode::NewClosureOnce || value.opcode == Format::Opcode::FunctionReference;
                    if (isSite(frame.values, value) || (names && value.immediates[0] < m_functions.size() && m_distanceToSite[value.immediates[0]] < distance))
                        leads.quickSet(i);
                }
            }
            for (bool changed = true; changed;) {
                changed = false;
                for (unsigned i = frame.blocks.size(); i--;) {
                    if (leads.quickGet(i))
                        continue;
                    for (auto& target : slice(m_targets, frame.blocks[i].targets)) {
                        if (target.block < frame.blocks.size() && leads.quickGet(target.block)) {
                            leads.quickSet(i);
                            changed = true;
                            break;
                        }
                    }
                }
            }
            return leads;
        }).iterator->value;
    }

    void prepareGlobalObject()
    {
        m_globalObject = JSGlobalObject::create(m_vm, JSGlobalObject::createStructure(m_vm, jsNull()));
        m_globalObject->setEvalEnabled(false, "The evaluator of texts compiles nothing"_s);
        m_unknown = constructEmptyObject(m_vm, m_globalObject->nullPrototypeObjectStructure());
        m_regExpConstructor = m_globalObject->regExpConstructor();
        auto remove = [&](ASCIILiteral holder, bool fromPrototype, ASCIILiteral name) {
            JSValue function = attempt([&]() -> JSValue {
                JSValue object = m_globalObject->get(m_globalObject, Identifier::fromString(m_vm, holder));
                if (fromPrototype && object.isObject())
                    object = object.get(m_globalObject, m_vm.propertyNames->prototype);
                return object.isObject() ? object.get(m_globalObject, Identifier::fromString(m_vm, name)) : JSValue();
            });
            if (function && function.isObject())
                m_functionsNotToBeUsed.append(function);
        };
        remove("Math"_s, false, "random"_s);
        remove("String"_s, true, "localeCompare"_s);
        remove("String"_s, true, "toLocaleLowerCase"_s);
        remove("String"_s, true, "toLocaleUpperCase"_s);
        remove("Number"_s, true, "toLocaleString"_s);
        remove("Array"_s, true, "toLocaleString"_s);
        remove("Object"_s, true, "toLocaleString"_s);
        m_objectAssign = attempt([&] {
            return m_globalObject->objectConstructor()->get(m_globalObject, Identifier::fromString(m_vm, "assign"_s));
        });
        JSValue math = attempt([&] {
            return m_globalObject->get(m_globalObject, Identifier::fromString(m_vm, "Math"_s));
        });
        if (math) {
            m_mathMin = attempt([&] {
                return math.get(m_globalObject, Identifier::fromString(m_vm, "min"_s));
            });
            m_mathMax = attempt([&] {
                return math.get(m_globalObject, Identifier::fromString(m_vm, "max"_s));
            });
        }
        m_objectKeys = attempt([&] {
            return m_globalObject->objectConstructor()->get(m_globalObject, Identifier::fromString(m_vm, "keys"_s));
        });
    }

    template<typename Functor>
    JSValue attempt(const Functor& functor)
    {
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(m_vm);
        JSValue result = functor();
        if (scope.exception()) [[unlikely]] {
            scope.clearException();
            return { };
        }
        return fromStored(result);
    }

    bool hasException()
    {
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(m_vm);
        return !!scope.exception();
    }

    JSValue toStored(JSValue value) const { return value ? value : JSValue(m_unknown); }
    JSValue fromStored(JSValue value) const { return value == JSValue(m_unknown) || (value && value.isObject() && m_functionsNotToBeUsed.contains(value)) ? JSValue() : value; }

    JSValue getGlobal(uint32_t name)
    {
        static constexpr ASCIILiteral allowed[] = {
            "Object"_s, "Array"_s, "String"_s, "Number"_s, "Boolean"_s, "Math"_s, "JSON"_s, "Map"_s, "Set"_s, "WeakMap"_s, "WeakSet"_s, "RegExp"_s, "Symbol"_s,
            "Error"_s, "TypeError"_s, "RangeError"_s, "SyntaxError"_s, "parseInt"_s, "parseFloat"_s, "isNaN"_s, "isFinite"_s, "NaN"_s, "Infinity"_s, "undefined"_s,
            "encodeURIComponent"_s, "decodeURIComponent"_s, "encodeURI"_s, "decodeURI"_s,
        };
        for (auto one : allowed) {
            if (isNamed(name, one)) {
                return attempt([&] {
                    return m_globalObject->get(m_globalObject, identifierOf(name));
                });
            }
        }
        return { };
    }

    JSObject* makeClosure(uint32_t function, Vector<JSValue>&& captures)
    {
        if (function >= m_functions.size() || m_closures.size() >= maxNumberOfClosures) {
            m_ranOutOfRecords |= function < m_functions.size();
            return nullptr;
        }
        m_closures.append(makeUniqueWithoutFastMallocCheck<Closure>(Closure { function, WTF::move(captures) }));
        Closure* closure = m_closures.last().get();
        JSObject* object = JSNativeStdFunction::create(m_vm, m_globalObject, m_functions[function].length, String(), [this, closure, isAlive = m_isAlive](JSGlobalObject*, CallFrame* callFrame) -> EncodedJSValue {
            if (!*isAlive)
                return JSValue::encode(jsUndefined());
            Vector<JSValue, 8> arguments;
            for (unsigned i = 0; i < callFrame->argumentCount(); ++i)
                arguments.append(fromStored(callFrame->uncheckedArgument(i)));
            return JSValue::encode(toStored(callClosure(*closure, fromStored(callFrame->thisValue()), arguments.span())));
        });
        m_closureOf.add(object, closure);
        if (auto shape = m_shapeOfConstructor.find(function); shape != m_shapeOfConstructor.end())
            makeClassObject(object, shape->value);
        else if (m_functions[function].kind == Format::FunctionKind::Plain && m_functions[function].flags & Format::FunctionHasThis) {
            JSObject* prototype = constructEmptyObject(m_globalObject);
            prototype->putDirect(m_vm, m_vm.propertyNames->constructor, object, static_cast<unsigned>(PropertyAttribute::DontEnum));
            object->putDirect(m_vm, m_vm.propertyNames->prototype, prototype, static_cast<unsigned>(PropertyAttribute::DontEnum));
        }
        return object;
    }

    JSObject* referenceTo(uint32_t function)
    {
        if (function >= m_functions.size() || m_functions[function].captures.count)
            return nullptr;
        auto found = m_references.find(function);
        if (found != m_references.end())
            return found->value;
        JSObject* object = makeClosure(function, { });
        if (object)
            m_references.add(function, object);
        return object;
    }

    void defineMethods(JSObject* object, uint32_t shape, bool areStatic)
    {
        for (auto& method : slice(m_methods, m_shapes[shape].methods)) {
            if (method.name == Format::none || !!(method.flags & Format::MethodIsStatic) != areStatic)
                continue;
            auto& name = identifierOf(method.name);
            JSObject* function = name.isNull() ? nullptr : referenceTo(method.function);
            if (!function)
                continue;
            if (method.kind == Format::MethodKind::Method) {
                object->putDirect(m_vm, name, function, static_cast<unsigned>(PropertyAttribute::DontEnum));
                continue;
            }
            JSValue existing = object->getDirect(m_vm, name);
            auto* accessor = existing ? dynamicDowncast<GetterSetter>(existing) : nullptr;
            JSObject* getter = method.kind == Format::MethodKind::Getter ? function : accessor && !accessor->isGetterNull() ? accessor->getter() : nullptr;
            JSObject* setter = method.kind == Format::MethodKind::Setter ? function : accessor && !accessor->isSetterNull() ? accessor->setter() : nullptr;
            object->putDirectAccessor(m_globalObject, name, GetterSetter::create(m_vm, m_globalObject, getter, setter), PropertyAttribute::Accessor | PropertyAttribute::DontEnum);
        }
    }

    JSObject* prototypeOf(uint32_t shape)
    {
        if (shape >= m_shapes.size() || m_shapes[shape].kind != Format::ShapeKind::Instance)
            return nullptr;
        auto found = m_prototypes.find(shape);
        if (found != m_prototypes.end())
            return found->value;
        JSObject* base = m_shapes[shape].base != Format::none ? prototypeOf(m_shapes[shape].base) : m_globalObject->objectPrototype();
        if (!base)
            return nullptr;
        JSObject* prototype = constructEmptyObject(m_globalObject, base);
        m_prototypes.add(shape, prototype);
        m_prototypesOfProgram.add(prototype);
        defineMethods(prototype, shape, false);
        return prototype;
    }

    void makeClassObject(JSObject* constructor, uint32_t shape)
    {
        JSObject* prototype = prototypeOf(shape);
        if (!prototype)
            return;
        constructor->putDirect(m_vm, m_vm.propertyNames->prototype, prototype, static_cast<unsigned>(PropertyAttribute::DontEnum));
        prototype->putDirect(m_vm, m_vm.propertyNames->constructor, constructor, static_cast<unsigned>(PropertyAttribute::DontEnum));
        defineMethods(constructor, shape, true);
    }

    JSValue newObjectWithShape(uint32_t shape)
    {
        if (shape >= m_shapes.size())
            return { };
        if (m_shapes[shape].kind != Format::ShapeKind::Instance)
            return constructEmptyObject(m_globalObject);
        if (m_shapes[shape].constructor < m_functions.size())
            referenceTo(m_shapes[shape].constructor);
        JSObject* prototype = prototypeOf(shape);
        return prototype ? JSValue(constructEmptyObject(m_globalObject, prototype)) : JSValue();
    }

    JSValue newArray(std::span<const JSValue> elements)
    {
        Vector<JSValue, 16> stored;
        for (JSValue element : elements)
            stored.append(toStored(element));
        return attempt([&] {
            return constructArray(m_globalObject, m_globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous), stored.span().data(), stored.size());
        });
    }

    bool isChoice(JSValue value) const { return value && value.isObject() && m_choices.contains(asObject(value)); }

    JSValue makeChoice(std::span<const JSValue> all)
    {
        Vector<JSValue, maxNumberOfChoices> different;
        for (JSValue one : all) {
            if (!one || one.isObject())
                return { };
            if (!different.containsIf([&](JSValue other) { return JSValue::strictEqual(m_globalObject, one, other); }))
                different.append(one);
        }
        if (different.size() == 1)
            return different[0];
        if (different.isEmpty() || different.size() > maxNumberOfChoices)
            return { };
        JSValue choice = newArray(different.span());
        if (choice)
            m_choices.add(asObject(choice));
        return choice;
    }

    Vector<JSValue, maxNumberOfChoices> choicesOf(JSValue value)
    {
        Vector<JSValue, maxNumberOfChoices> result;
        if (!isChoice(value)) {
            result.append(value);
            return result;
        }
        auto* array = uncheckedDowncast<JSArray>(asObject(value));
        for (unsigned i = 0; i < array->length(); ++i)
            result.append(array->tryGetIndexQuickly(i));
        return result;
    }

    template<typename Functor>
    JSValue forEachChoice(JSValue first, JSValue second, const Functor& functor)
    {
        Vector<JSValue, maxNumberOfChoices> all;
        for (JSValue a : choicesOf(first)) {
            for (JSValue b : choicesOf(second)) {
                if (all.size() == maxNumberOfChoices)
                    return { };
                JSValue one = functor(a, b);
                if (!one)
                    return { };
                all.append(one);
            }
        }
        return makeChoice(all.span());
    }

    JSValue boundBy(JSValue callee, std::span<const JSValue> arguments)
    {
        if (arguments.size() != 2 || (callee != m_mathMin && callee != m_mathMax))
            return { };
        if (isChoice(arguments[0]) || isChoice(arguments[1])) {
            if (!arguments[0] || !arguments[1])
                return { };
            return forEachChoice(arguments[0], arguments[1], [&](JSValue a, JSValue b) -> JSValue {
                if (!a.isNumber() || !b.isNumber() || std::isnan(a.asNumber()) || std::isnan(b.asNumber()))
                    return { };
                return jsNumber(callee == m_mathMin ? std::min(a.asNumber(), b.asNumber()) : std::max(a.asNumber(), b.asNumber()));
            });
        }
        if (callee != m_mathMin || !arguments[0] == !arguments[1])
            return { };
        JSValue bound = arguments[0] ? arguments[0] : arguments[1];
        if (!bound.isUInt32() || bound.asUInt32() >= maxNumberOfChoices)
            return { };
        Vector<JSValue, maxNumberOfChoices> all;
        for (unsigned i = 0; i <= bound.asUInt32(); ++i)
            all.append(jsNumber(i));
        return makeChoice(all.span());
    }

    JSValue callClosure(Closure& closure, JSValue thisValue, std::span<const JSValue> arguments)
    {
        auto& function = m_functions[closure.function];
        if (function.flags & (Format::FunctionIsAsync | Format::FunctionHasNewTarget) || closure.captures.size() != function.captures.count)
            return { };
        Vector<JSValue> passed;
        passed.appendVector(closure.captures);
        if (function.flags & Format::FunctionHasThis)
            passed.append(thisValue);
        unsigned numberOfParameters = function.parameterTypes.count;
        bool hasRest = function.flags & Format::FunctionHasRestParameter && numberOfParameters;
        unsigned numberOfPlainParameters = numberOfParameters - hasRest;
        for (unsigned i = 0; i < numberOfPlainParameters; ++i)
            passed.append(i < arguments.size() ? arguments[i] : jsUndefined());
        if (hasRest)
            passed.append(newArray(arguments.size() > numberOfPlainParameters ? arguments.subspan(numberOfPlainParameters) : std::span<const JSValue>()));
        return invokeWhateverItIs(closure.function, WTF::move(passed));
    }

    JSValue invokeWhateverItIs(uint32_t index, Vector<JSValue>&& passed)
    {
        if (m_functions[index].flags & Format::FunctionIsAsync) {
            if (!(m_functions[index].flags & Format::FunctionIsGenerator) && m_distanceToSite[index] <= maxDistanceToSite)
                invoke(index, WTF::move(passed));
            return { };
        }
        if (!(m_functions[index].flags & Format::FunctionIsGenerator))
            return invoke(index, WTF::move(passed));
        m_yielded.append(Vector<JSValue>());
        invoke(index, WTF::move(passed));
        auto yielded = m_yielded.takeLast();
        JSValue array = m_stepsLeft ? newArray(yielded.span()) : JSValue();
        if (!array)
            return { };
        JSValue method = attempt([&] {
            return array.get(m_globalObject, m_vm.propertyNames->iteratorSymbol);
        });
        return call(method, array, { });
    }

    unsigned m_looked { 0 };

    uint64_t sizeOfWalk(JSValue value, UncheckedKeyHashMap<JSObject*, uint64_t>& known)
    {
        constexpr uint64_t most = std::numeric_limits<uint32_t>::max();
        if (!value || !value.isObject())
            return 0;
        JSObject* object = asObject(value);
        if (object->isCallable())
            return 1;
        auto found = known.find(object);
        if (found != known.end())
            return std::min(found->value, most);
        if (known.size() >= maxNumberOfObjectsSized || !m_vm.isSafeToRecurse())
            return most;
        known.add(object, std::numeric_limits<uint64_t>::max());
        uint64_t size = 1;
        ++m_looked;
        Vector<PropertyOffset, 16> offsets;
        object->structure()->forEachProperty(m_vm, [&](const PropertyTableEntry& entry) -> bool {
            if (!(entry.attributes() & PropertyAttribute::DontEnum) && !entry.key()->isSymbol())
                offsets.append(entry.offset());
            return true;
        });
        for (PropertyOffset offset : offsets) {
            ++m_looked;
            size = std::min(size + 1 + sizeOfWalk(object->getDirect(offset), known), most);
            if (size == most)
                return most;
        }
        if (hasIndexedProperties(object->indexingType())) {
            for (unsigned i = 0, length = object->getVectorLength(); i < length; ++i) {
                ++m_looked;
                if (object->canGetIndexQuickly(i))
                    size = std::min(size + sizeOfWalk(object->getIndexQuickly(i), known), most);
                if (size == most)
                    return most;
            }
        }
        known.set(object, size);
        return size;
    }

    unsigned costOf(JSValue value)
    {
        if (!value || !value.isCell())
            return 0;
        if (value.isString())
            return asString(value)->length() / 64;
        if (!value.isObject())
            return 0;
        UncheckedKeyHashMap<JSObject*, uint64_t> known;
        uint64_t size = sizeOfWalk(value, known);
        if (size >= std::numeric_limits<uint32_t>::max())
            return costOfWhatIsTooLargeToSay;
        uint64_t cost = size / 64;
        if (auto* array = dynamicDowncast<JSArray>(value))
            cost += array->length() / 4;
        return static_cast<unsigned>(cost);
    }

    bool refuseWhatIsTooLargeToSay()
    {
        m_stepsLeft -= std::min(m_stepsLeft, std::max(m_looked, 1u));
        return false;
    }

    bool isCheapToConvert(JSValue value)
    {
        if (!value || !value.isObject())
            return true;
        m_looked = 0;
        unsigned cost = costOf(value);
        if (cost == costOfWhatIsTooLargeToSay)
            return refuseWhatIsTooLargeToSay();
        m_stepsLeft -= std::min(m_stepsLeft, cost);
        return m_stepsLeft;
    }

    bool fillArguments(MarkedArgumentBuffer& list, JSValue thisValue, std::span<const JSValue> arguments)
    {
        m_looked = 0;
        unsigned cost = costOf(thisValue);
        if (cost == costOfWhatIsTooLargeToSay)
            return refuseWhatIsTooLargeToSay();
        for (JSValue argument : arguments) {
            if (argument && argument.isNumber() && argument.asNumber() > maxNumberHandedToBuiltin && std::isfinite(argument.asNumber()))
                return false;
            if (argument && argument.isObject()) {
                JSValue length = asObject(argument)->getDirect(m_vm, m_vm.propertyNames->length);
                if (length && length.isNumber() && length.asNumber() > maxNumberHandedToBuiltin)
                    return false;
            }
            unsigned costOfArgument = costOf(argument);
            if (costOfArgument == costOfWhatIsTooLargeToSay)
                return refuseWhatIsTooLargeToSay();
            cost += std::min(costOfArgument, costOfWhatIsTooLargeToSay - cost);
            list.append(toStored(argument));
        }
        m_stepsLeft -= std::min(m_stepsLeft, cost);
        return !list.hasOverflowed() && m_stepsLeft && m_vm.isSafeToRecurse();
    }

    JSValue call(JSValue callee, JSValue thisValue, std::span<const JSValue> arguments)
    {
        if (!callee || !callee.isObject())
            return { };
        if (auto* closure = m_closureOf.get(asObject(callee)))
            return m_functions[closure->function].kind == Format::FunctionKind::Constructor ? JSValue() : callClosure(*closure, thisValue, arguments);
        if (callee == JSValue(m_regExpConstructor))
            return makeRegExp(arguments);
        if (JSValue bounded = boundBy(callee, arguments))
            return bounded;
        if (isChoice(thisValue) || std::ranges::any_of(arguments, [&](JSValue argument) { return isChoice(argument); }))
            return { };
        auto callData = JSC::getCallData(callee);
        MarkedArgumentBuffer list;
        if (callData.type == CallData::Type::None || !thisValue || !fillArguments(list, thisValue, arguments))
            return { };
        return attempt([&] {
            return JSC::call(m_globalObject, callee, callData, thisValue, list);
        });
    }

    JSValue construct(JSValue callee, std::span<const JSValue> arguments)
    {
        if (!callee || !callee.isObject())
            return { };
        if (auto* closure = m_closureOf.get(asObject(callee))) {
            auto shape = m_shapeOfConstructor.find(closure->function);
            JSValue object;
            if (shape != m_shapeOfConstructor.end())
                object = newObjectWithShape(shape->value);
            else if (JSValue prototype = asObject(callee)->getDirect(m_vm, m_vm.propertyNames->prototype); prototype && m_functions[closure->function].kind == Format::FunctionKind::Plain)
                object = constructEmptyObject(m_globalObject, prototype.isObject() ? asObject(prototype) : m_globalObject->objectPrototype());
            if (!object)
                return { };
            JSValue result = callClosure(*closure, object, arguments);
            return result && result.isObject() ? result : object;
        }
        if (callee == JSValue(m_regExpConstructor))
            return makeRegExp(arguments);
        auto constructData = JSC::getConstructData(callee);
        MarkedArgumentBuffer list;
        if (constructData.type == CallData::Type::None || !fillArguments(list, jsUndefined(), arguments))
            return { };
        return attempt([&]() -> JSValue {
            return JSC::construct(m_globalObject, callee, constructData, list);
        });
    }

    std::optional<Vector<JSValue>> elementsOf(JSValue iterable)
    {
        Vector<JSValue> result;
        if (!iterable)
            return std::nullopt;
        if (auto* array = dynamicDowncast<JSArray>(iterable)) {
            unsigned length = array->length();
            if (length > maxNumberHandedToBuiltin)
                return std::nullopt;
            for (unsigned i = 0; i < length; ++i) {
                JSValue element = attempt([&] {
                    return array->getIndex(m_globalObject, i);
                });
                result.append(element);
            }
            return result;
        }
        JSValue iteration = iteratorOpen(iterable);
        if (!iteration)
            return std::nullopt;
        for (;;) {
            JSValue hasNext = iteratorHasNext(iteration);
            if (!hasNext || !m_stepsLeft)
                return std::nullopt;
            --m_stepsLeft;
            if (!hasNext.asBoolean())
                return result;
            result.append(m_iterations[iteration.asUInt32()].current);
        }
    }

    JSValue iteratorOpen(JSValue iterable)
    {
        if (!iterable)
            return { };
        JSValue method = attempt([&] {
            return iterable.get(m_globalObject, m_vm.propertyNames->iteratorSymbol);
        });
        JSValue iterator = call(method, iterable, { });
        if (!iterator || !iterator.isObject())
            return { };
        JSValue next = attempt([&] {
            return iterator.get(m_globalObject, m_vm.propertyNames->next);
        });
        if (!next)
            return { };
        m_iterations.append(Iteration { iterator, next, { } });
        return jsNumber(m_iterations.size() - 1);
    }

    JSValue iteratorHasNext(JSValue iteration)
    {
        if (!iteration || !iteration.isUInt32() || iteration.asUInt32() >= m_iterations.size())
            return { };
        unsigned index = iteration.asUInt32();
        JSValue result = call(m_iterations[index].next, m_iterations[index].iterator, { });
        if (!result || !result.isObject())
            return { };
        JSValue done = attempt([&] {
            return result.get(m_globalObject, m_vm.propertyNames->done);
        });
        if (!done)
            return { };
        bool isDone = done.toBoolean(m_globalObject);
        m_iterations[index].current = isDone ? JSValue() : attempt([&] {
            return result.get(m_globalObject, m_vm.propertyNames->value);
        });
        return jsBoolean(!isDone);
    }

    std::optional<String> toText(JSValue value)
    {
        if (!value || !isCheapToConvert(value))
            return std::nullopt;
        JSValue string = attempt([&]() -> JSValue {
            return value.toString(m_globalObject);
        });
        if (!string || !string.isString() || asString(string)->length() > maxConstantTextSize)
            return std::nullopt;
        String text;
        if (!attempt([&]() -> JSValue {
            text = asString(string)->value(m_globalObject);
            return jsUndefined();
        }))
            return std::nullopt;
        return text;
    }

    JSValue makeRegExp(std::span<const JSValue> arguments, bool isHandedOver = true)
    {
        JSValue source = arguments.size() ? arguments[0] : jsUndefined();
        JSValue flags = arguments.size() > 1 ? arguments[1] : jsUndefined();
        if (isChoice(source)) {
            for (JSValue one : choicesOf(source))
                makeRegExp(std::array { one, flags }, isHandedOver);
            return { };
        }
        if (isChoice(flags))
            flags = { };
        String text;
        String flagsText = emptyString();
        if (auto* object = source ? dynamicDowncast<RegExpObject>(source) : nullptr) {
            text = object->regExp()->pattern();
            flagsText = String { Yarr::flagsString(object->regExp()->flags()).span() };
        } else if (source && source.isUndefined())
            text = emptyString();
        else if (auto known = toText(source))
            text = WTF::move(*known);
        else {
            remarkOnUnknownText(source);
            return { };
        }
        if (!flags) {
            if (isHandedOver)
                handOver(text, { });
            return { };
        }
        if (!flags.isUndefined()) {
            auto known = toText(flags);
            if (!known)
                return { };
            flagsText = WTF::move(*known);
        }
        auto parsed = Yarr::parseFlags(flagsText);
        if (!parsed)
            return { };
        if (isHandedOver)
            handOver(text, *parsed);
        RegExp* regExp = RegExp::create(m_vm, text, *parsed);
        if (!regExp->isValid())
            return { };
        return RegExpObject::create(m_vm, m_globalObject->regExpStructure(), regExp);
    }

    void handOver(const String& source, OptionSet<Yarr::Flags> flags)
    {
        if (m_remarks) [[unlikely]]
            m_remarkOfSite.set(m_site, std::pair { std::numeric_limits<uint32_t>::max(), makeString("known: "_s, source.length(), " characters, hash "_s, source.hash()) });
        unsigned& count = source.length() > ImageRegExp::maxLengthWithCode ? m_numberOfExpressionsWithoutCode : m_numberOfExpressions;
        if (count == (source.length() > ImageRegExp::maxLengthWithCode ? maxNumberOfExpressionsWithoutCode : maxNumberOfExpressions) || !m_handedOver.add(makeString(flags.toRaw(), '/', source)).isNewEntry)
            return;
        ++count;
        if (m_remarks && source.length() > 4096)
            dataLogLn("AOT: evaluator of texts: a RegExp in f", m_functionsEntered.isEmpty() ? 0 : m_functionsEntered.last(), ", come to from f", m_functionsEntered.isEmpty() ? 0 : m_functionsEntered.first(), ": a text of ", source.length(), " characters is known");
        m_builder.requestRegExp(m_vm, source, flags, true);
    }

    JSValue getModuleVariable(uint32_t index)
    {
        if (index >= m_variables.size())
            return { };
        if (m_isStored.quickGet(index))
            return m_variables[index];
        uint32_t module = m_variablesOfProgram[index].module;
        uint32_t initializer = module < m_modules.size() ? m_modules[module].initializer : Format::none;
        if (initializer < m_functions.size() && !m_wasEntered.get(initializer) && m_stepsLeftForModules) {
            unsigned stepsLeft = m_stepsLeft;
            unsigned stepsInReserve = std::exchange(m_stepsInReserve, 0);
            m_stepsLeft = std::min(m_stepsLeftForModules, maxNumberOfSteps / 8);
            m_stepsLeftForModules -= m_stepsLeft;
            invoke(initializer, { });
            if (m_remarks && !m_stepsLeft)
                dataLogLn("AOT: evaluator of texts: the module of f", initializer, ", run for a variable of it, was cut short: no steps left");
            m_stepsLeftForModules += m_stepsLeft;
            m_stepsLeft = stepsLeft;
            m_stepsInReserve = stepsInReserve;
            if (m_isStored.quickGet(index))
                return m_variables[index];
        }
        return jsUndefined();
    }

    JSValue operate(const Format::Value& value, std::span<const JSValue> arguments)
    {
        auto is = [&](ASCIILiteral name) {
            return isNamed(value.immediates[0], name);
        };
        auto argument = [&](unsigned index) {
            return index < arguments.size() ? arguments[index] : jsUndefined();
        };
        auto isObject = [&](unsigned index) {
            return index < arguments.size() && arguments[index] && arguments[index].isObject();
        };
        if (is("DefineField"_s) || is("PutByValDirect"_s) || is("PrivateFieldAdd"_s) || is("DefineMethod"_s)) {
            if (!isObject(0) || !argument(1) || !isCheapToConvert(argument(1)))
                return { };
            return attempt([&]() -> JSValue {
                auto name = argument(1).toPropertyKey(m_globalObject);
                if (hasException())
                    return { };
                asObject(argument(0))->putDirectMayBeIndex(m_globalObject, name, toStored(argument(2)));
                return argument(0);
            });
        }
        if (is("RegExpLiteral"_s))
            return makeRegExp(arguments, false);
        if (is("HasOriginalStructure"_s) || is("IsOriginalArray"_s))
            return jsBoolean(false);
        if (is("AppendSpread"_s)) {
            auto* array = argument(0) ? dynamicDowncast<JSArray>(argument(0)) : nullptr;
            auto elements = array ? elementsOf(argument(1)) : std::nullopt;
            if (!elements)
                return { };
            return attempt([&]() -> JSValue {
                for (JSValue element : *elements)
                    array->push(m_globalObject, toStored(element));
                return array;
            });
        }
        if (is("Apply"_s) || is("CallWithSpread"_s)) {
            auto elements = elementsOf(argument(2));
            return elements ? call(argument(0), argument(1), elements->span()) : JSValue();
        }
        if (is("Call"_s) || is("CallNoInline"_s))
            return arguments.size() >= 2 ? call(arguments[0], arguments[1], arguments.subspan(2)) : JSValue();
        if (is("ConstructWithArray"_s)) {
            auto elements = elementsOf(argument(1));
            return elements ? construct(argument(0), elements->span()) : JSValue();
        }
        if (is("CopyDataProperties"_s))
            return call(m_objectAssign, jsUndefined(), std::array { argument(0), argument(1) });
        if (is("CloneObject"_s) || is("CloneObjectWith"_s))
            return call(m_objectAssign, jsUndefined(), std::array { JSValue(constructEmptyObject(m_globalObject)), argument(0) });
        if (is("CopyDataPropertiesExcluding"_s)) {
            JSValue copy = call(m_objectAssign, jsUndefined(), std::array { JSValue(constructEmptyObject(m_globalObject)), argument(0) });
            if (!copy || !copy.isObject())
                return { };
            return attempt([&]() -> JSValue {
                for (JSValue key : arguments.subspan(1)) {
                    if (!key || !isCheapToConvert(key))
                        return m_unknown;
                    auto name = key.toPropertyKey(m_globalObject);
                    if (hasException())
                        return { };
                    JSCell::deleteProperty(asObject(copy), m_globalObject, name);
                }
                return copy;
            });
        }
        if (is("ForInKeys"_s))
            return argument(0) && argument(0).isUndefinedOrNull() ? newArray({ }) : call(m_objectKeys, jsUndefined(), std::array { argument(0) });
        if (is("HasEnumerableProperty"_s)) {
            if (!isObject(0) || !argument(1) || !isCheapToConvert(argument(1)))
                return { };
            return attempt([&]() -> JSValue {
                auto name = argument(1).toPropertyKey(m_globalObject);
                if (hasException())
                    return { };
                return jsBoolean(asObject(argument(0))->hasProperty(m_globalObject, name));
            });
        }
        if (is("GetTemplateObject"_s)) {
            if (!isObject(0) || !isObject(1))
                return { };
            asObject(argument(0))->putDirect(m_vm, m_vm.propertyNames->raw, argument(1), static_cast<unsigned>(PropertyAttribute::DontEnum));
            return argument(0);
        }
        if (is("ParseJSON"_s)) {
            auto text = toText(argument(0));
            if (!text)
                return { };
            m_stepsLeft -= std::min<unsigned>(m_stepsLeft, text->length() / 16);
            return attempt([&] {
                return JSONParseWithException(m_globalObject, *text);
            });
        }
        if (is("IsCallable"_s))
            return argument(0) ? jsBoolean(argument(0).isCallable()) : JSValue();
        if (is("IsObject"_s))
            return argument(0) ? jsBoolean(argument(0).isObject()) : JSValue();
        if (is("ConstructorResult"_s) || is("DerivedConstructorResult"_s))
            return isObject(0) ? argument(0) : argument(1);
        if (is("NewPrivateName"_s))
            return Symbol::create(m_vm);
        if (is("PrivateGet"_s)) {
            if (!isObject(0) || !argument(1) || !argument(1).isSymbol())
                return { };
            return attempt([&] {
                return argument(0).get(m_globalObject, Identifier::fromUid(asSymbol(argument(1))->privateName()));
            });
        }
        if (is("PrivateSet"_s)) {
            if (isObject(0) && argument(1) && argument(1).isSymbol())
                asObject(argument(0))->putDirect(m_vm, Identifier::fromUid(asSymbol(argument(1))->privateName()), toStored(argument(2)));
            return jsUndefined();
        }
        if (is("SetFunctionName"_s))
            return jsUndefined();
        return { };
    }

    JSValue evaluate(Frame& frame, uint32_t id)
    {
        auto& value = frame.values[id];
        auto child = [&](unsigned index) -> JSValue {
            uint32_t operand = value.children[index];
            return operand < frame.slots.size() ? frame.slots[operand] : JSValue();
        };
        auto arguments = [&] {
            Vector<JSValue, 8> result;
            for (uint32_t argument : slice(m_words, value.arguments))
                result.append(argument < frame.slots.size() ? frame.slots[argument] : JSValue());
            return result;
        };
        auto isPrimitive = [](JSValue operand) {
            return operand && !operand.isObject() && !operand.isSymbol() && !operand.isBigInt();
        };
        auto compute = [&](auto&& result) -> JSValue {
            if (isChoice(child(0)) || isChoice(child(1))) {
                if (!child(0) || !child(1))
                    return { };
                return forEachChoice(child(0), child(1), [&](JSValue a, JSValue b) -> JSValue {
                    return isPrimitive(a) && isPrimitive(b) ? jsNumber(result(a.toNumber(m_globalObject), b.toNumber(m_globalObject))) : JSValue();
                });
            }
            if (!isPrimitive(child(0)) || !isPrimitive(child(1)))
                return { };
            return jsNumber(result(child(0).toNumber(m_globalObject), child(1).toNumber(m_globalObject)));
        };
        if (!m_choices.isEmpty()) [[unlikely]] {
            bool hasChoice = false;
            forEachOperand(value, [&](uint32_t operand) {
                hasChoice |= operand < frame.slots.size() && isChoice(frame.slots[operand]);
            });
            if (hasChoice) {
                switch (value.opcode) {
                case Format::Opcode::Add:
                case Format::Opcode::Sub:
                case Format::Opcode::Mul:
                case Format::Opcode::Strcat:
                case Format::Opcode::ToString:
                case Format::Opcode::Cast:
                case Format::Opcode::Call:
                case Format::Opcode::CallMethod:
                case Format::Opcode::CallDirect:
                case Format::Opcode::Construct:
                case Format::Opcode::NewClosure:
                case Format::Opcode::NewClosureOnce:
                case Format::Opcode::PutModuleVariable:
                case Format::Opcode::PutField:
                case Format::Opcode::PutById:
                    break;
                default:
                    return { };
                }
            }
        }
        auto computeInt32 = [&](auto&& result) -> JSValue {
            if (!isPrimitive(child(0)) || !isPrimitive(child(1)))
                return { };
            return jsNumber(result(JSC::toInt32(child(0).toNumber(m_globalObject)), JSC::toInt32(child(1).toNumber(m_globalObject))));
        };
        auto compare = [&](auto&& holds) -> JSValue {
            if (!child(0) || !child(1) || !isCheapToConvert(child(0)) || !isCheapToConvert(child(1)))
                return { };
            return attempt([&]() -> JSValue {
                return jsBoolean(holds(child(0), child(1)));
            });
        };
        auto keyed = [&](auto&& functor) -> JSValue {
            if (!child(0) || !child(1) || !isCheapToConvert(child(1)))
                return { };
            return attempt([&]() -> JSValue {
                auto name = child(1).toPropertyKey(m_globalObject);
                if (hasException())
                    return { };
                return functor(name);
            });
        };
        uint64_t bits = static_cast<uint64_t>(value.immediates[1]) << 32 | value.immediates[0];
        switch (value.opcode) {
        case Format::Opcode::Undefined:
        case Format::Opcode::LoopHint:
            return jsUndefined();
        case Format::Opcode::Null:
            return jsNull();
        case Format::Opcode::Boolean:
            return jsBoolean(value.immediates[0]);
        case Format::Opcode::Integer:
            return jsNumber(static_cast<double>(static_cast<int64_t>(bits)));
        case Format::Opcode::Double:
            return jsNumber(purifyNaN(std::bit_cast<double>(bits)));
        case Format::Opcode::String:
            return textOf(value.immediates[0]);
        case Format::Opcode::FunctionReference:
            return referenceTo(value.immediates[0]);
        case Format::Opcode::NewClosure:
        case Format::Opcode::NewClosureOnce: {
            Vector<JSValue> captures;
            captures.appendVector(arguments());
            return makeClosure(value.immediates[0], WTF::move(captures));
        }
        case Format::Opcode::GetModuleVariable:
            return getModuleVariable(value.immediates[0]);
        case Format::Opcode::PutModuleVariable:
            if (value.immediates[0] < m_variables.size()) {
                m_variables[value.immediates[0]] = child(0);
                m_isStored.quickSet(value.immediates[0]);
            }
            return jsUndefined();
        case Format::Opcode::GetGlobal:
        case Format::Opcode::GetGlobalForTypeOf:
            return getGlobal(value.immediates[0]);
        case Format::Opcode::NewObject:
            return constructEmptyObject(m_globalObject);
        case Format::Opcode::NewObjectWithShape:
            return newObjectWithShape(value.immediates[0]);
        case Format::Opcode::GetField:
        case Format::Opcode::GetById: {
            Identifier name = value.opcode == Format::Opcode::GetField ? nameOfField(value) : identifierOf(value.immediates[0]);
            if (!child(0) || name.isNull())
                return { };
            return attempt([&] {
                return child(0).get(m_globalObject, name);
            });
        }
        case Format::Opcode::PutField:
        case Format::Opcode::PutById: {
            Identifier name = value.opcode == Format::Opcode::PutField ? nameOfField(value) : identifierOf(value.immediates[0]);
            if (child(0) && child(0).isObject() && !name.isNull()) {
                attempt([&]() -> JSValue {
                    PutPropertySlot slot(child(0));
                    child(0).put(m_globalObject, name, toStored(child(1)), slot);
                    return jsUndefined();
                });
            }
            return jsUndefined();
        }
        case Format::Opcode::InById:
            if (!child(0) || !child(0).isObject() || identifierOf(value.immediates[0]).isNull())
                return { };
            return attempt([&]() -> JSValue {
                return jsBoolean(asObject(child(0))->hasProperty(m_globalObject, identifierOf(value.immediates[0])));
            });
        case Format::Opcode::DeleteById:
            if (!child(0) || !child(0).isObject() || identifierOf(value.immediates[0]).isNull())
                return { };
            return attempt([&]() -> JSValue {
                return jsBoolean(JSCell::deleteProperty(asObject(child(0)), m_globalObject, identifierOf(value.immediates[0])));
            });
        case Format::Opcode::GetByVal:
            if (child(0) && child(1) && child(1).isUInt32()) {
                return attempt([&] {
                    return child(0).get(m_globalObject, child(1).asUInt32());
                });
            }
            return keyed([&](const Identifier& name) {
                return child(0).get(m_globalObject, name);
            });
        case Format::Opcode::PutByVal:
            if (child(0) && child(0).isObject() && child(1) && child(1).isUInt32()) {
                attempt([&]() -> JSValue {
                    asObject(child(0))->putByIndexInline(m_globalObject, child(1).asUInt32(), toStored(child(2)), false);
                    return jsUndefined();
                });
            } else if (child(0) && child(0).isObject()) {
                keyed([&](const Identifier& name) -> JSValue {
                    PutPropertySlot slot(child(0));
                    child(0).put(m_globalObject, name, toStored(child(2)), slot);
                    return jsUndefined();
                });
            }
            return jsUndefined();
        case Format::Opcode::InByVal:
            if (!child(0) || !child(0).isObject())
                return { };
            return keyed([&](const Identifier& name) -> JSValue {
                return jsBoolean(asObject(child(0))->hasProperty(m_globalObject, name));
            });
        case Format::Opcode::DeleteByVal:
            if (!child(0) || !child(0).isObject())
                return { };
            return keyed([&](const Identifier& name) -> JSValue {
                return jsBoolean(JSCell::deleteProperty(asObject(child(0)), m_globalObject, name));
            });
        case Format::Opcode::NewArray:
            return newArray(arguments().span());
        case Format::Opcode::Add:
            if (!child(0) || !child(1))
                return { };
            if (isChoice(child(0)) || isChoice(child(1))) {
                return forEachChoice(child(0), child(1), [&](JSValue a, JSValue b) {
                    return attempt([&] {
                        return isCheapToConvert(a) && isCheapToConvert(b) ? jsAdd(m_globalObject, a, b) : JSValue();
                    });
                });
            }
            return attempt([&] {
                return isCheapToConvert(child(0)) && isCheapToConvert(child(1)) ? jsAdd(m_globalObject, child(0), child(1)) : JSValue();
            });
        case Format::Opcode::Sub:
            return compute([](double a, double b) { return a - b; });
        case Format::Opcode::Mul:
            return compute([](double a, double b) { return a * b; });
        case Format::Opcode::Div:
            return compute([](double a, double b) { return a / b; });
        case Format::Opcode::Mod:
            return compute([](double a, double b) { return Math::fmodDouble(a, b); });
        case Format::Opcode::Pow:
            return compute([](double a, double b) { return operationMathPow(a, b); });
        case Format::Opcode::BitAnd:
            return computeInt32([](int32_t a, int32_t b) { return a & b; });
        case Format::Opcode::BitOr:
            return computeInt32([](int32_t a, int32_t b) { return a | b; });
        case Format::Opcode::BitXor:
            return computeInt32([](int32_t a, int32_t b) { return a ^ b; });
        case Format::Opcode::LShift:
            return computeInt32([](int32_t a, int32_t b) { return static_cast<int32_t>(static_cast<uint32_t>(a) << (b & 31)); });
        case Format::Opcode::RShift:
            return computeInt32([](int32_t a, int32_t b) { return a >> (b & 31); });
        case Format::Opcode::URShift:
            return computeInt32([](int32_t a, int32_t b) { return static_cast<uint32_t>(a) >> (b & 31); });
        case Format::Opcode::Negate:
            return isPrimitive(child(0)) ? jsNumber(-child(0).toNumber(m_globalObject)) : JSValue();
        case Format::Opcode::ToNumber:
        case Format::Opcode::ToNumeric:
            return isPrimitive(child(0)) ? jsNumber(child(0).toNumber(m_globalObject)) : JSValue();
        case Format::Opcode::BitNot:
            return isPrimitive(child(0)) ? jsNumber(~JSC::toInt32(child(0).toNumber(m_globalObject))) : JSValue();
        case Format::Opcode::Less:
            return compare([&](JSValue a, JSValue b) { return jsLess<true>(m_globalObject, a, b); });
        case Format::Opcode::LessEq:
            return compare([&](JSValue a, JSValue b) { return jsLessEq<true>(m_globalObject, a, b); });
        case Format::Opcode::Greater:
            return compare([&](JSValue a, JSValue b) { return jsLess<false>(m_globalObject, b, a); });
        case Format::Opcode::GreaterEq:
            return compare([&](JSValue a, JSValue b) { return jsLessEq<false>(m_globalObject, b, a); });
        case Format::Opcode::StrictEq:
            return compare([&](JSValue a, JSValue b) { return JSValue::strictEqual(m_globalObject, a, b); });
        case Format::Opcode::Eq:
            return compare([&](JSValue a, JSValue b) { return JSValue::equal(m_globalObject, a, b); });
        case Format::Opcode::LogicalNot:
            return child(0) ? jsBoolean(!child(0).toBoolean(m_globalObject)) : JSValue();
        case Format::Opcode::ToBoolean:
            return child(0) ? jsBoolean(child(0).toBoolean(m_globalObject)) : JSValue();
        case Format::Opcode::TypeOf:
            return child(0) ? JSValue(jsTypeStringForValue(m_globalObject, child(0))) : JSValue();
        case Format::Opcode::InstanceOf:
            if (!child(0) || !child(1) || !child(1).isObject())
                return { };
            return attempt([&]() -> JSValue {
                return jsBoolean(asObject(child(1))->hasInstance(m_globalObject, child(0)));
            });
        case Format::Opcode::ToString:
            if (!child(0))
                return { };
            if (isChoice(child(0))) {
                return forEachChoice(child(0), jsUndefined(), [&](JSValue a, JSValue) {
                    return attempt([&]() -> JSValue {
                        return isCheapToConvert(a) ? JSValue(a.toString(m_globalObject)) : JSValue();
                    });
                });
            }
            return attempt([&]() -> JSValue {
                return isCheapToConvert(child(0)) ? JSValue(child(0).toString(m_globalObject)) : JSValue();
            });
        case Format::Opcode::ToPropertyKey:
            if (!child(0))
                return { };
            return attempt([&] {
                return isCheapToConvert(child(0)) ? child(0).toPropertyKeyValue(m_globalObject) : JSValue();
            });
        case Format::Opcode::IsUndefinedOrNull:
            return child(0) ? jsBoolean(child(0).isUndefinedOrNull()) : JSValue();
        case Format::Opcode::Strcat: {
            auto parts = arguments();
            if (std::ranges::any_of(parts, [&](JSValue part) { return isChoice(part); })) {
                JSValue result = jsEmptyString(m_vm);
                for (JSValue part : parts) {
                    if (!part || !result)
                        return { };
                    result = forEachChoice(result, part, [&](JSValue a, JSValue b) {
                        return attempt([&] {
                            return isCheapToConvert(a) && isCheapToConvert(b) ? jsAdd(m_globalObject, a, b) : JSValue();
                        });
                    });
                }
                return result;
            }
            return attempt([&]() -> JSValue {
                JSString* result = jsEmptyString(m_vm);
                for (JSValue part : parts) {
                    if (!part || !isCheapToConvert(part))
                        return m_unknown;
                    JSString* text = part.toString(m_globalObject);
                    if (hasException())
                        return { };
                    result = jsString(m_globalObject, result, text);
                    if (hasException())
                        return { };
                }
                return result;
            });
        }
        case Format::Opcode::IteratorOpen:
            return iteratorOpen(child(0));
        case Format::Opcode::IteratorHasNext:
            return iteratorHasNext(child(0));
        case Format::Opcode::IteratorValue:
            return child(0) && child(0).isUInt32() && child(0).asUInt32() < m_iterations.size() ? m_iterations[child(0).asUInt32()].current : JSValue();
        case Format::Opcode::IteratorClose:
            return jsUndefined();
        case Format::Opcode::Call:
            noteSite(frame, id);
            return call(child(0), child(1), arguments().span());
        case Format::Opcode::Construct:
            noteSite(frame, id);
            return construct(child(0), arguments().span());
        case Format::Opcode::CallMethod: {
            auto& name = identifierOf(value.immediates[0]);
            if (!child(0) || name.isNull())
                return { };
            JSValue method = attempt([&] {
                return child(0).get(m_globalObject, name);
            });
            return call(method, child(0), arguments().span());
        }
        case Format::Opcode::CallDirect: {
            Vector<JSValue> passed;
            passed.appendVector(arguments());
            if (value.immediates[0] >= m_functions.size())
                return { };
            return invokeWhateverItIs(value.immediates[0], WTF::move(passed));
        }
        case Format::Opcode::Cast:
            return child(0);
        case Format::Opcode::Yield:
            if (m_yielded.isEmpty())
                return { };
            m_yielded.last().append(child(0));
            return jsUndefined();
        case Format::Opcode::GetResumeMode:
            return jsNumber(0);
        case Format::Opcode::Operation:
            return operate(value, arguments().span());
        default:
            return { };
        }
    }

    void noteSite(const Frame& frame, uint32_t id)
    {
        if (m_remarks) [[unlikely]]
            m_site = static_cast<uint64_t>(frame.function) << 32 | id;
    }

    ASCIILiteral describe(JSValue value)
    {
        if (!value)
            return "something not known that nothing made: a parameter"_s;
        if (value.isUndefined())
            return "undefined"_s;
        if (value.isNull())
            return "null"_s;
        if (value.isBoolean())
            return "a boolean"_s;
        if (value.isNumber())
            return "a number"_s;
        if (value.isString())
            return "a text"_s;
        if (!value.isObject())
            return "a symbol or a bigint"_s;
        if (m_closureOf.contains(asObject(value)))
            return "a closure"_s;
        if (value.isCallable())
            return "a function of the engine"_s;
        if (value.inherits<RegExpObject>())
            return "a RegExp"_s;
        if (value.inherits<JSArray>())
            return "an array"_s;
        if (value.inherits<JSSet>())
            return "a Set"_s;
        if (value.inherits<JSMap>())
            return "a Map"_s;
        return "an object"_s;
    }

    uint32_t numberOfCharactersIn(JSValue value, unsigned depth = 0)
    {
        if (!value || !value.isCell())
            return 0;
        if (value.isString())
            return asString(value)->length();
        auto* array = depth < 3 ? dynamicDowncast<JSArray>(value) : nullptr;
        if (!array)
            return 0;
        uint64_t sum = 0;
        unsigned length = std::min(array->length(), 1u << 20);
        for (unsigned i = 0; i < length; ++i) {
            if (JSValue element = array->tryGetIndexQuickly(i))
                sum += numberOfCharactersIn(element, depth + 1);
        }
        return static_cast<uint32_t>(std::min<uint64_t>(sum, std::numeric_limits<uint32_t>::max() - 1));
    }

    template<typename Functor>
    void forEachOperand(const Format::Value& value, const Functor& functor)
    {
        for (uint32_t child : value.children) {
            if (child != Format::none)
                functor(child);
        }
        for (uint32_t argument : slice(m_words, value.arguments))
            functor(argument);
    }

    void takeNote(Frame& frame, uint32_t id)
    {
        Note note = m_pendingNote;
        auto& value = frame.values[id];
        uint64_t sum = 0;
        uint32_t first = Format::none;
        forEachOperand(value, [&](uint32_t operand) {
            if (operand >= frame.slots.size())
                return;
            if (first == Format::none)
                first = operand;
            if (!frame.slots[operand]) {
                if (!note.origin)
                    note.origin = frame.notes[operand].origin;
                sum += frame.notes[operand].length;
            } else
                sum += numberOfCharactersIn(frame.slots[operand]);
        });
        if (!note.origin) {
            String name;
            JSValue receiver = first != Format::none ? frame.slots[first] : JSValue();
            if (value.opcode == Format::Opcode::Operation)
                name = decode(bytesOf(value.immediates[0]));
            else if ((value.opcode == Format::Opcode::CallMethod || value.opcode == Format::Opcode::GetById) && receiver && !receiver.isUndefinedOrNull() && !identifierOf(value.immediates[0]).isNull()) {
                JSValue prototype = attempt([&]() -> JSValue {
                    return receiver.isObject() ? asObject(receiver)->getPrototype(m_globalObject) : JSValue(receiver.synthesizePrototype(m_globalObject));
                });
                bool isPublic = prototype && prototype.isObject() && !m_prototypesOfProgram.contains(asObject(prototype)) && attempt([&]() -> JSValue {
                    return jsBoolean(asObject(prototype)->hasProperty(m_globalObject, identifierOf(value.immediates[0])));
                }) == jsBoolean(true);
                if (isPublic)
                    name = identifierOf(value.immediates[0]).string();
            } else if (value.opcode == Format::Opcode::GetGlobal)
                name = "a global"_s;
            m_origins.append(Origin { frame.function, id, first != Format::none ? describe(receiver) : "no operand"_s, WTF::move(name) });
            note.origin = m_origins.size();
        }
        note.length = std::max(note.length, static_cast<uint32_t>(std::min<uint64_t>(sum, std::numeric_limits<uint32_t>::max() - 1)));
        frame.notes[id] = note;
        m_pendingNote = note;
    }

    void remarkOnUnknownText(JSValue source)
    {
        if (!m_remarks)
            return;
        Note note = m_noteOfSource;
        StringPrintStream out;
        if (source)
            out.print("it is ", describe(source), ", of which no text can be made");
        else if (!note.origin)
            out.print("an argument of the function the walk began with");
        else {
            auto& origin = m_origins[note.origin - 1];
            auto values = slice(m_values, m_functions[origin.function].values);
            out.print(nameOf(static_cast<Opcode>(values[origin.value].opcode)), origin.name.isNull() ? "" : " ", origin.name.isNull() ? String(emptyString()) : origin.name, " in f", origin.function, ", its first operand: ", origin.operand);
        }
        out.print("; ", note.length, " characters so far");
        auto& remark = m_remarkOfSite.add(m_site, std::pair { 0u, String() }).iterator->value;
        if (remark.second.isNull() || remark.first < note.length)
            remark = { note.length, makeString("not known: "_s, out.toString()) };
    }

    void remarkOnWalk(uint32_t from)
    {
        if (m_remarks && (!m_stepsLeft || m_ranOutOfRecords))
            dataLogLn("AOT: evaluator of texts: the walk from f", from, " was cut short: ", !m_stepsLeft ? "no steps left" : "", !m_stepsLeft && m_ranOutOfRecords ? ", " : "", m_ranOutOfRecords ? "no closures left" : "", "; the longest text it made: ", m_longestTextOfWalk, " characters");
        m_ranOutOfRecords = false;
        m_longestTextOfWalk = 0;
    }

    void remarkOnSites()
    {
        if (!m_remarks)
            return;
        dataLogLn("AOT: evaluator of texts: ", static_cast<unsigned>((MonotonicTime::now() - m_start).milliseconds()), " ms in all; ", m_closures.size(), " closures made");
        for (uint32_t index = 0; index < m_functions.size(); ++index) {
            if (m_distanceToSite[index])
                continue;
            auto& function = m_functions[index];
            auto values = slice(m_values, function.values);
            for (uint32_t id = 0; id < values.size(); ++id) {
                if (!isSite(values, values[id]))
                    continue;
                auto found = m_remarkOfSite.find(static_cast<uint64_t>(index) << 32 | id);
                dataLogLn("AOT: evaluator of texts: site f", index, " v", id, ": ", found != m_remarkOfSite.end() ? found->value.second : String(m_wasEntered.get(index) ? "the function was entered, the site never come to"_s : "the function was never entered"_s),
                    " (captures ", function.captures.count, ", parameters ", function.parameterTypes.count, function.kind == Format::FunctionKind::Constructor ? ", a constructor" : function.kind == Format::FunctionKind::Method ? ", a method" : function.kind == Format::FunctionKind::Module ? ", a module" : "", function.flags & Format::FunctionHasRestParameter ? ", a rest parameter" : "", function.flags & Format::FunctionIsAsync ? ", async" : "", function.flags & Format::FunctionIsGenerator ? ", a generator" : "", function.flags & Format::FunctionIsTyped ? "" : ", not typed", ")");
            }
        }
    }

    void pass(Frame& frame, uint32_t block, std::span<const JSValue> arguments, std::span<const Note> notes)
    {
        auto parameters = slice(m_words, frame.blocks[block].parameters);
        for (unsigned i = 0; i < parameters.size(); ++i) {
            if (parameters[i] >= frame.slots.size())
                continue;
            frame.slots[parameters[i]] = i < arguments.size() ? arguments[i] : jsUndefined();
            if (m_remarks)
                frame.notes[parameters[i]] = i < notes.size() ? notes[i] : Note();
        }
    }

    JSValue invoke(uint32_t index, Vector<JSValue>&& passed)
    {
        auto& function = m_functions[index];
        auto blocks = slice(m_blocks, function.blocks);
        if (m_depth >= maxDepth || blocks.empty() || !m_stepsLeft || !m_vm.isSafeToRecurse())
            return { };
        SetForScope depth(m_depth, m_depth + 1);
        m_wasEntered.set(index);
        m_functionsEntered.append(index);
        auto leave = makeScopeExit([&] {
            m_functionsEntered.removeLast();
        });
        m_stepsLeft -= std::min<unsigned>(m_stepsLeft, function.values.count / 8);
        Frame frame { index, slice(m_values, function.values), blocks, Vector<JSValue>(function.values.count), { } };
        if (m_remarks)
            frame.notes.grow(function.values.count);
        pass(frame, 0, passed.span(), { });
        return runFrom(frame, 0, function.values.count <= maxSizeToGoBothWays ? maxNumberOfBranchesGoneBothWays : 0);
    }

    JSValue runFrom(Frame& frame, uint32_t at, unsigned branchesToGoBothWays)
    {
        unsigned numberOfGuesses = 0;
        for (;;) {
            if (at >= frame.blocks.size())
                return { };
            auto& block = frame.blocks[at];
            for (uint32_t id : slice(m_words, block.values)) {
                if (!m_stepsLeft || id >= frame.values.size())
                    return { };
                --m_stepsLeft;
                if (m_remarks) [[unlikely]] {
                    m_pendingNote = { };
                    auto& value = frame.values[id];
                    if (value.opcode == Format::Opcode::Construct || value.opcode == Format::Opcode::Call) {
                        auto arguments = slice(m_words, value.arguments);
                        m_noteOfSource = arguments.size() && arguments[0] < frame.notes.size() ? frame.notes[arguments[0]] : Note();
                    }
                }
                JSValue result = evaluate(frame, id);
                if (result && result.isString()) {
                    unsigned length = asString(result)->length();
                    if (length > maxConstantTextSize)
                        result = { };
                    else if (length > minLengthOfLongText) {
                        m_hasMadeLongText = true;
                        m_longestTextOfWalk = std::max(m_longestTextOfWalk, length);
                    }
                    if (m_stepsInReserve && result && frame.values[id].opcode != Format::Opcode::String) {
                        unsigned granted = std::min(m_stepsInReserve, 64 + length);
                        m_stepsInReserve -= granted;
                        m_stepsLeft += granted;
                    }
                }
                frame.slots[id] = result;
                if (!result && m_remarks) [[unlikely]]
                    takeNote(frame, id);
            }
            if (!m_stepsLeft)
                return { };
            --m_stepsLeft;
            auto targets = slice(m_targets, block.targets);
            auto enter = [&](unsigned index, Frame& into) -> uint32_t {
                if (index >= targets.size() || targets[index].block >= frame.blocks.size())
                    return std::numeric_limits<uint32_t>::max();
                Vector<JSValue, 8> arguments;
                Vector<Note, 8> notes;
                for (uint32_t argument : slice(m_words, targets[index].arguments)) {
                    arguments.append(argument < frame.slots.size() ? frame.slots[argument] : JSValue());
                    if (m_remarks)
                        notes.append(argument < frame.notes.size() ? frame.notes[argument] : Note());
                }
                pass(into, targets[index].block, arguments.span(), notes.span());
                return targets[index].block;
            };
            JSValue subject = block.value < frame.slots.size() ? frame.slots[block.value] : JSValue();
            if (block.terminator != Format::TerminatorKind::Return && isChoice(subject))
                subject = { };
            unsigned taken = 0;
            switch (block.terminator) {
            case Format::TerminatorKind::Return:
                if (block.value == Format::none)
                    return jsUndefined();
                if (!subject && m_remarks && block.value < frame.notes.size())
                    m_pendingNote = frame.notes[block.value];
                return subject;
            case Format::TerminatorKind::Throw:
            case Format::TerminatorKind::Unreachable:
                return { };
            case Format::TerminatorKind::Jump:
            case Format::TerminatorKind::Invoke:
                break;
            case Format::TerminatorKind::Branch:
                if (subject)
                    taken = !subject.toBoolean(m_globalObject);
                else {
                    if (++numberOfGuesses > maxNumberOfGuesses || targets.size() < 2 || targets[0].block >= frame.blocks.size())
                        return { };
                    auto kind = frame.blocks[targets[0].block].terminator;
                    taken = kind == Format::TerminatorKind::Throw || kind == Format::TerminatorKind::Unreachable;
                    auto& leads = blocksLeadingToSites(frame);
                    if (targets[1].block < frame.blocks.size() && leads.quickGet(targets[0].block) != leads.quickGet(targets[1].block))
                        taken = leads.quickGet(targets[1].block);
                    if (branchesToGoBothWays) {
                        --branchesToGoBothWays;
                        Frame copy = frame;
                        uint32_t other = enter(!taken, copy);
                        runFrom(copy, other, branchesToGoBothWays);
                    }
                }
                break;
            case Format::TerminatorKind::Switch: {
                if (targets.empty() || ((!subject || !subject.isNumber()) && ++numberOfGuesses > maxNumberOfGuesses))
                    return { };
                taken = targets.size() - 1;
                auto cases = slice(m_words, block.cases);
                for (unsigned i = 0; i + 1 < cases.size() && subject && subject.isNumber(); i += 2) {
                    if (static_cast<double>(static_cast<int64_t>(static_cast<uint64_t>(cases[i + 1]) << 32 | cases[i])) == subject.asNumber()) {
                        taken = i / 2;
                        break;
                    }
                }
                break;
            }
            }
            at = enter(taken, frame);
        }
    }

    VM& m_vm;
    const Program& m_program;
    ImageBuilder& m_builder;
    std::span<const uint32_t> m_words;
    std::span<const uint8_t> m_bytes;
    std::span<const Format::String> m_strings;
    std::span<const Format::Shape> m_shapes;
    std::span<const Format::Field> m_fields;
    std::span<const Format::Method> m_methods;
    std::span<const Format::Variable> m_variablesOfProgram;
    std::span<const Format::Module> m_modules;
    std::span<const Format::Function> m_functions;
    std::span<const Format::Block> m_blocks;
    std::span<const Format::Value> m_values;
    std::span<const Format::Target> m_targets;
    JSGlobalObject* m_globalObject { nullptr };
    JSObject* m_unknown { nullptr };
    Vector<JSValue, 8> m_functionsNotToBeUsed;
    JSObject* m_regExpConstructor { nullptr };
    JSValue m_objectAssign;
    JSValue m_objectKeys;
    JSValue m_mathMin;
    JSValue m_mathMax;
    UncheckedKeyHashSet<JSObject*> m_choices;
    Box<bool> m_isAlive { Box<bool>::create(true) };
    Vector<JSValue> m_variables;
    BitVector m_isStored;
    Vector<JSString*> m_texts;
    Vector<Identifier> m_identifiers;
    Vector<std::unique_ptr<Closure>> m_closures;
    UncheckedKeyHashMap<JSObject*, Closure*> m_closureOf;
    UncheckedKeyHashMap<uint32_t, JSObject*, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_references;
    UncheckedKeyHashMap<uint32_t, JSObject*, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_prototypes;
    UncheckedKeyHashSet<JSObject*> m_prototypesOfProgram;
    UncheckedKeyHashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_shapeOfConstructor;
    Vector<Iteration> m_iterations;
    Vector<Vector<JSValue>> m_yielded;
    BitVector m_wasEntered;
    Vector<uint8_t> m_distanceToSite;
    UncheckedKeyHashMap<uint32_t, BitVector, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_blocksLeadingToSites;
    bool m_hasSites { false };
    bool m_hasMadeLongText { false };
    const bool m_remarks { Options::verboseAOTCompilation() };
    Vector<Origin> m_origins;
    Note m_pendingNote;
    Note m_noteOfSource;
    uint64_t m_site { 0 };
    UncheckedKeyHashMap<uint64_t, std::pair<uint32_t, String>, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_remarkOfSite;
    bool m_ranOutOfRecords { false };
    unsigned m_longestTextOfWalk { 0 };
    Vector<uint32_t, 48> m_functionsEntered;
    UncheckedKeyHashSet<String> m_handedOver;
    unsigned m_numberOfExpressions { 0 };
    unsigned m_numberOfExpressionsWithoutCode { 0 };
    unsigned m_stepsLeft { maxNumberOfSteps };
    unsigned m_stepsInReserve { 0 };
    MonotonicTime m_start { MonotonicTime::now() };
    unsigned m_stepsLeftForModules { maxNumberOfSteps * 8 };
    unsigned m_depth { 0 };
};

// RegExp::compile() finds the code by source and flags, however the RegExp was made. So nothing depends on what is found here but whether there is code to find.
void addRegExpLiterals(VM& vm, const Program& program, ImageBuilder& builder)
{
    auto& builtins = Builtins::singleton();
    for (auto& function : program.functions) {
        for (auto& block : function.blocks) {
            for (ValueID id : block.values) {
                auto& value = function.at(id);
                if (value.opcode != Opcode::CallBuiltin || value.arguments.isEmpty() || value.arguments.size() > 2)
                    continue;
                // Whichever entry: in code without types nothing says that the arguments are strings.
                auto& builtin = builtins.at(value.builtin());
                bool isRegExpNew = builtin.implementation.kind == BuiltinImplementation::Inline && builtin.implementation.inlined == InlineBuiltin::RegExpNew;
                if (!isRegExpNew && ((builtin.kind != BuiltinKind::Function && builtin.kind != BuiltinKind::Constructor) || builtin.name != "RegExp"_s))
                    continue;
                Vector<uint8_t> source;
                if (!appendConstantText(program, function, value.arguments[0], source))
                    continue;
                String pattern = EvaluatorOfTexts::decode(source.span());
                if (pattern.isNull())
                    continue;
                PossibleFlags all;
                if (value.arguments.size() == 2)
                    appendPossibleFlags(program, function, value.arguments[1], all, 4);
                else
                    all.append(OptionSet<Yarr::Flags> { });
                for (auto flags : all)
                    builder.requestRegExp(vm, pattern, flags);
            }
        }
    }
    EvaluatorOfTexts(vm, program, builder).run();
    builder.didRequestAllRegExps();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
