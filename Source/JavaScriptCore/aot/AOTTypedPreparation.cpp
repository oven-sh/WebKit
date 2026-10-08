/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedPasses.h"

#if ENABLE(AOT)

#include "Options.h"
#include <wtf/MonotonicTime.h>
#include <wtf/NumberOfCores.h>
#include <wtf/Threading.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringView.h>

namespace JSC { namespace AOT { namespace Typed {

static String keyOf(std::span<const uint8_t> bytes)
{
    return bytes.empty() ? emptyString() : String(byteCast<Latin1Character>(bytes));
}

std::optional<uint32_t> Layout::fieldNamed(StringID name) const
{
    for (uint32_t i = 0; i < fields.size(); ++i) {
        if (fields[i].name == name)
            return i;
    }
    return std::nullopt;
}

uint32_t Function::allocationOf(Allocation&& allocation)
{
    size_t found = allocations.find(allocation);
    if (found == notFound) {
        found = allocations.size();
        allocations.append(WTF::move(allocation));
    }
    return found;
}

bool Program::isUpcast(LayoutID from, LayoutID to) const
{
    if (from == to || at(to).admitsForeignObjects)
        return true;
    return !at(from).admitsForeignObjects && std::ranges::includes(at(to).members, at(from).members);
}

std::optional<StringID> Program::find(std::span<const uint8_t> bytes) const
{
    auto iterator = m_stringIDs.find(keyOf(bytes));
    if (iterator == m_stringIDs.end())
        return std::nullopt;
    return StringID(iterator->value);
}

StringID Program::intern(std::span<const uint8_t> bytes)
{
    auto result = m_stringIDs.add(keyOf(bytes), static_cast<uint32_t>(strings.size()));
    if (result.isNewEntry) {
        m_ownStrings.append(Vector<uint8_t>(bytes));
        strings.append(m_ownStrings.last().span());
        noteString(StringID(result.iterator->value));
    }
    return StringID(result.iterator->value);
}

void Program::replaceString(std::span<const uint8_t> from, std::span<const uint8_t> to)
{
    RELEASE_ASSERT(m_stringIDs.isEmpty());
    for (auto& string : strings) {
        if (!equalSpans(string, from))
            continue;
        m_ownStrings.append(Vector<uint8_t>(to));
        string = m_ownStrings.last().span();
    }
}

Vector<StringID> Program::genericAccessIdentifiers() const
{
    BitVector seen;
    Vector<StringID> result;
    for (auto& function : functions) {
        for (auto& value : function.values) {
            if (value.opcode != Opcode::GetById && value.opcode != Opcode::PutById && value.opcode != Opcode::CallMethod)
                continue;
            // `#name`, and a name with half a surrogate pair, are no UTF-8: such an access has no cache, and the run time makes out the name.
            if (!seen.set(value.string().index()) && !String::fromUTF8(string(value.string())).isNull())
                result.append(value.string());
        }
    }
    return result;
}

void Program::noteString(StringID id)
{
    BuiltinName name = Builtins::singleton().nameOf(string(id));
    m_builtinNames.append(name);
    if (!name)
        return;
    if (m_stringsOfBuiltinNames.size() <= name.index())
        m_stringsOfBuiltinNames.grow(name.index() + 1);
    m_stringsOfBuiltinNames[name.index()] = id;
}

class Preparation {
public:
    explicit Preparation(Program& program)
        : m_program(program)
    {
    }

    void run()
    {
        for (uint32_t i = 0; i < m_program.strings.size(); ++i)
            m_program.m_stringIDs.add(keyOf(m_program.strings[i]), i);
        m_program.emptyString = m_program.intern({ });

        m_program.closureLayout = LayoutID(m_program.layouts.size());
        Layout closure;
        closure.kind = LayoutKind::Closure;
        closure.isAllocated = true;
        m_program.layouts.append(WTF::move(closure));
    }

private:
    Program& m_program;
};

void prepare(Program& program)
{
    Preparation(program).run();
}

void linkAllocations(Program& program)
{
    UncheckedKeyHashMap<unsigned, Vector<uint32_t, 1>, IntHash<unsigned>, WTF::UnsignedWithZeroKeyHashTraits<unsigned>> byHash;
    auto numberOf = [&](const Allocation& allocation) -> uint32_t {
        Hasher hasher;
        add(hasher, allocation.layout, allocation.constructedBy, allocation.born);
        for (uint16_t field : allocation.order)
            add(hasher, field);
        auto& candidates = byHash.add(hasher.hash(), Vector<uint32_t, 1>()).iterator->value;
        for (uint32_t candidate : candidates) {
            if (program.allocations[candidate] == allocation)
                return candidate;
        }
        candidates.append(program.allocations.size());
        program.allocations.append(allocation);
        return candidates.last();
    };
    // Closures, which no value names the layout of, are allocated with number 0.
    numberOf(Allocation { program.closureLayout, { }, { }, 0 });
    for (auto& function : program.functions) {
        for (auto& block : function.blocks) {
            for (ValueID id : block.values) {
                auto& value = function.at(id);
                if (value.opcode != Opcode::NewObjectWithShape)
                    continue;
                auto& allocation = function.allocations[value.allocation()];
                program.at(allocation.layout).isAllocated = true;
                value.immediates[1] = numberOf(allocation);
            }
        }
        function.allocations.clear();
    }
    // `new (F as any)()`, Reflect.construct(F, ..), a bound F: whoever constructs does not know the function, and leaves the object to the
    // run time. It is born with nothing, like the one `new` makes for a function it knows, and gets its prototype when it is made.
    for (unsigned i = 0; i < program.functions.size(); ++i) {
        auto& function = program.functions[i];
        if (function.kind != FunctionKind::Plain || function.declaredAs || !function.thisType || !program.genericEntrypoints.contains(i))
            continue;
        LayoutID view = program.at(function.thisType).onlyView();
        if (view && program.at(view).kind == LayoutKind::Record && program.at(view).isAllocated)
            function.allocationOfThis = numberOf(Allocation { view, { }, { }, 0 });
    }
}

template<typename Functor>
static void forEachIndexInParallel(unsigned count, const Functor& functor)
{
    std::atomic<unsigned> next { 0 };
    auto work = [&] {
        for (unsigned i = next++; i < count; i = next++)
            functor(i);
    };
    Vector<Ref<Thread>> threads;
    for (int i = 1; i < WTF::numberOfProcessorCores(); ++i)
        threads.append(Thread::create("AOT optimizer"_s, [&work] { work(); }));
    work();
    for (auto& thread : threads)
        thread->waitForCompletion();
}

Vector<String> optimize(Program& program)
{
    MonotonicTime start = MonotonicTime::now();
    auto didFinish = [&](ASCIILiteral what) {
        dataLogLnIf(Options::verboseAOTCompilation(), "AOT passes: ", what, " done at ", (MonotonicTime::now() - start).seconds(), " s");
    };
    prepare(program);
    splitCoroutines(program);
    createGenericEntrypoints(program);
    assignAllocations(program);
    findViewsRetaggedTo(program);
    program.types.freeze();
    didFinish("what looks at the whole program"_s);

    // Each function is worked on as a copy. What it takes from others, to inline them, is as it was read, whoever gets there first.
    unsigned count = program.functions.size();
    Vector<Function> optimized(count);
    Vector<Vector<String>> findings(count);
    forEachIndexInParallel(count, [&](unsigned i) {
        FunctionID id(i);
        Function function = program.functions[i];
        verify(program, id, function, findings[i]);
        if (!findings[i].isEmpty())
            return;
        cleanUp(program, function);
        for (unsigned round = 0; round < 8; ++round) {
            unsigned changed = resolveCalls(program, function);
            changed += inlineCalls(program, id, function);
            changed += simplifyCasts(program, function);
            changed += removeRedundantCatchBlocks(program, function);
            if (!changed)
                break;
            cleanUp(program, function);
        }
        resolveBuiltins(program, function);
        verify(program, id, function, findings[i]);
        optimized[i] = WTF::move(function);
    });
    didFinish("what looks at one function"_s);

    Vector<String> result;
    for (auto& ofFunction : findings)
        result.appendVector(WTF::move(ofFunction));
    if (!result.isEmpty())
        return result;
    program.functions = WTF::move(optimized);
    linkAllocations(program);
    computeEffectSummaries(program);
    didFinish("allocations and effects"_s);
    return result;
}

Vector<String> describeForMap(const Program& program)
{
    unsigned count = program.functions.size();
    Vector<FunctionID> entered;
    entered.fill(FunctionID(), count);
    for (auto& entry : program.genericEntrypoints)
        entered[entry.value.index()] = FunctionID(entry.key);
    Vector<LayoutID> classes;
    classes.fill(LayoutID(), count);
    for (unsigned i = 0; i < program.layouts.size(); ++i) {
        auto& layout = program.layouts[i];
        if (layout.kind != LayoutKind::Instance)
            continue;
        for (auto* methods : { &layout.methods, &layout.staticMethods }) {
            for (auto& method : *methods)
                classes[method.function.index()] = LayoutID(i);
        }
    }
    for (unsigned i = 0; i < count; ++i) {
        if (FunctionID resume = program.functions[i].resumeFunction)
            classes[resume.index()] = classes[i];
    }
    auto text = [&](StringID id) {
        return id ? String::fromUTF8(program.string(id)) : String();
    };
    auto entrypointOf = [&](FunctionID id) -> ASCIILiteral {
        auto& function = program.at(id);
        if (id == program.entry)
            return "entry"_s;
        if (function.kind == FunctionKind::Module)
            return "module"_s;
        if (function.resumeFunction)
            return "ramp"_s;
        if (function.environment && program.at(function.environment).isCoroutineFrame)
            return "resume"_s;
        return function.isTyped() ? "typed"_s : "untyped"_s;
    };
    Vector<String> result;
    for (unsigned i = 0; i < count; ++i) {
        auto& function = program.functions[i];
        FunctionID described = entered[i] ? entered[i] : FunctionID(i);
        LayoutID itsClass = classes[described.index()];
        String className = itsClass ? text(program.at(itsClass).name) : String();
        String path = function.module ? text(program.modules[function.module.index()].path) : String();
        result.append(makeString(className, className.isEmpty() ? ""_s : "."_s, text(function.name), '\t', entered[i] ? "generic-"_s : ""_s, entrypointOf(described), '\t', i, '\t', path, '\t', function.position));
    }
    return result;
}

bool dumpAtStage(std::span<const uint8_t> bytes, const String& stage, PrintStream& out)
{
    if (stage == "builtins"_s) {
        static constexpr ASCIILiteral kinds[] = { "Function"_s, "Constructor"_s, "StaticFunction"_s, "Constant"_s, "Method"_s, "Getter"_s, "Setter"_s, "Operation"_s };
        auto& builtins = Builtins::singleton();
        for (unsigned i = 0; i < builtins.size(); ++i) {
            auto& builtin = builtins.at(BuiltinID(i));
            out.print(kinds[static_cast<unsigned>(builtin.kind)], " ", builtin.holder.isEmpty() ? "-"_s : builtin.holder, " ", builtin.name, " ", builtin.numberOfRequiredParameters(), " ", builtin.isUnknown() ? "unknown" : "known");
            if (builtin.implementation.kind == BuiltinImplementation::Number)
                out.printf(" %016llx", static_cast<unsigned long long>(std::bit_cast<uint64_t>(builtin.implementation.number)));
            out.print("\n");
        }
        return true;
    }
    ASCIILiteral whyFailed;
    auto program = Program::tryRead(bytes, whyFailed);
    if (!program) {
        out.print(whyFailed);
        return false;
    }
    if (stage == "read"_s) {
        program->dump(out);
        return true;
    }
    if (stage == "optimized"_s) {
        auto findings = optimize(*program);
        program->dump(out);
        for (auto& finding : findings)
            out.print("not valid: ", finding, "\n");
        return true;
    }
    // "functions:12,34": those functions, once the passes have run.
    if (stage.startsWith("functions:"_s)) {
        auto findings = optimize(*program);
        for (auto part : StringView(stage).substring(10).split(',')) {
            uint32_t number = 0;
            for (unsigned i = 0; i < part.length(); ++i)
                number = number * 10 + (part[i] - '0');
            if (number < program->functions.size())
                program->dump(out, FunctionID(number));
        }
        for (auto& finding : findings)
            out.print("not valid: ", finding, "\n");
        return true;
    }
    out.print("there is no such stage");
    return false;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
