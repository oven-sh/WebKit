/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedPasses.h"

#include "AOTFunction.h"
#include "AOTTypedThreads.h"

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

namespace {

uint32_t knownHeaderNameOf(std::span<const uint8_t> text)
{
    if (text.empty())
        return unknownHeaderName;
    for (uint8_t character : text) {
        if (!isASCIIAlphanumeric(character) && !std::ranges::contains("!#$%&'*+-.^_`|~"_s.span(), static_cast<char>(character)))
            return unknownHeaderName;
    }
    auto isNamed = [&](ASCIILiteral lowercase) {
        if (text.size() != lowercase.length())
            return false;
        for (size_t i = 0; i < text.size(); ++i) {
            if (toASCIILower(text[i]) != static_cast<uint8_t>(lowercase[i]))
                return false;
        }
        return true;
    };
#define RETURN_NUMBER_IF_NAMED(enumerator, lowercase, number) \
    if (isNamed(lowercase ## _s)) \
        return number;
    FOR_EACH_AOT_TYPED_HTTP_HEADER_NAME(RETURN_NUMBER_IF_NAMED)
#undef RETURN_NUMBER_IF_NAMED
    return uncommonHeaderName;
}

class BuiltinSpecialization {
public:
    enum class Mode : uint8_t {
        Prelude,
        Speculation,
    };

    BuiltinSpecialization(const Program& program, Function& function, Mode mode)
        : m_program(program)
        , m_function(function)
        , m_builtins(Builtins::singleton())
        , m_mode(mode)
    {
    }

    void useStubsInColdCode()
    {
        BitVector isInLoop = blocksInLoops(m_function);
        m_isStaticallyCold.fill(true, m_function.blocks.size());
        for (size_t block : isInLoop)
            m_isStaticallyCold[block] = false;
    }

    unsigned run(CleanUpTiming timing = CleanUpTiming::Immediate)
    {
        recordRefutedReceivers();
        unsigned changed = m_mode == Mode::Speculation ? keepHeadersInHolders() : 0;
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            for (unsigned i = m_remainingValueCounts[b]; i < m_function.blocks[b].values.size(); ++i) {
                auto& value = m_function.at(m_function.blocks[b].values[i]);
                if (value.opcode == Opcode::Operation && value.builtin() == m_builtins.callIntrinsic()) {
                    convertCallIntrinsicToCall(value);
                    ++changed;
                    continue;
                }
                m_siteBlock = BlockID(b);
                if ((m_mode == Mode::Speculation && (unboxDateTimes(BlockID(b), i) || readNumberOnEachPath(BlockID(b), i))) || specialize(BlockID(b), i)) {
                    ++changed;
                    break;
                }
            }
            if (unsigned remaining = std::exchange(m_remainingValueCounts[b], 0))
                m_function.blocks[b].values.removeAt(0, remaining);
        }
        if (changed && timing == CleanUpTiming::Immediate)
            cleanUp(m_program, m_function);
        return changed;
    }

private:
    struct Site {
        ValueID receiver;
        StringID name;
    };

    struct Refuted {
        ValueID receiver;
        uint32_t engineClass { Format::none };
    };

    const Type& typeOf(ValueID value) const { return m_program.typeOf(m_function, value); }

    ValueID originOf(ValueID value) const
    {
        return m_program.originOf(m_function, value);
    }

    std::optional<Site> siteOf(const Value& value) const
    {
        if (value.opcode == Opcode::CallMethod)
            return Site { value.child(0), value.string() };
        if (value.opcode != Opcode::Call)
            return std::nullopt;
        auto& read = m_function.at(originOf(value.child(0)));
        if (read.opcode != Opcode::GetById || originOf(read.child(0)) != originOf(value.child(1)))
            return std::nullopt;
        return Site { value.child(1), read.string() };
    }

    BuiltinName globalNameOf(ValueID id) const
    {
        auto& value = m_function.at(originOf(id));
        if (value.opcode != Opcode::GetGlobal)
            return { };
        BuiltinName name = m_program.builtinName(value.string());
        return m_builtins.isGlobal(name) ? name : BuiltinName();
    }

    BuiltinID firstGlobalEntryCalledBy(const Value& value) const
    {
        switch (value.opcode) {
        case Opcode::CallMethod:
            return m_builtins.findStatic(BuiltinKind::StaticFunction, globalNameOf(value.child(0)), m_program.builtinName(value.string()));
        case Opcode::Call: {
            if (BuiltinName global = globalNameOf(value.child(0)))
                return m_builtins.find(BuiltinKind::Function, { }, global);
            auto site = siteOf(value);
            return site ? m_builtins.findStatic(BuiltinKind::StaticFunction, globalNameOf(site->receiver), m_program.builtinName(site->name)) : BuiltinID();
        }
        case Opcode::Construct:
            if (BuiltinName global = globalNameOf(value.child(0)))
                return m_builtins.findConstructor(global);
            return { };
        default:
            return { };
        }
    }

    FunctionID preludeFunctionOf(BuiltinID entry) const
    {
        if (!entry || m_builtins.at(entry).implementation.kind != BuiltinImplementation::Prelude)
            return { };
        return m_program.preludeFunctions[static_cast<unsigned>(m_builtins.at(entry).implementation.prelude)];
    }

    bool mayMakeSetWithStringKeys(const Value& construct) const
    {
        if (construct.hasStringKeysByType)
            return true;
        auto isStringOrNullish = [](const Type& type) {
            return type.primitives & Primitive::String && type.hasOnlyPrimitives(Primitive::String | Primitive::UndefinedOrNull);
        };
        ValueID given = construct.arguments[0];
        auto& list = m_function.at(originOf(given));
        if (list.opcode == Opcode::NewArray && !list.arguments.isEmpty()) {
            return std::ranges::all_of(list.arguments, [&](ValueID item) {
                return isStringOrNullish(typeOf(item));
            });
        }
        for (TypeID type : { m_function.at(given).type, m_function.at(given).prediction(), list.prediction() }) {
            if (!type)
                continue;
            auto* engineClass = m_program.at(type).singleEngineClass();
            if (engineClass && engineClass->type == ArrayType && m_program.at(type).elements && isStringOrNullish(m_program.at(m_program.at(type).elements)))
                return true;
        }
        return false;
    }

    FunctionID functionThatFillsSet(BuiltinID entry, const Value& construct) const
    {
        BuiltinID selectedHere = entry ? m_builtins.next(entry) : BuiltinID();
        if (!selectedHere || m_builtins.at(selectedHere).implementation.kind != BuiltinImplementation::Prelude || m_builtins.at(selectedHere).implementation.prelude != PreludeFunction::SetConstruct || mayMakeSetWithStringKeys(construct))
            return { };
        return preludeFunctionOf(selectedHere);
    }

    FunctionID preludeFunctionFor(BuiltinID entry, const Value& call) const
    {
        FunctionID callee = preludeFunctionOf(entry);
        if (!callee || call.arguments.isEmpty())
            return callee;
        BuiltinName callback = globalNameOf(call.arguments[0]);
        if (!callback)
            return callee;
        static const BuiltinName booleanConstructor = Builtins::singleton().nameOf(asBytes("Boolean"_s.span8()));
        static const BuiltinName numberConstructor = Builtins::singleton().nameOf(asBytes("Number"_s.span8()));
        PreludeFunction which = m_builtins.at(entry).implementation.prelude;
        FunctionID withoutCalls;
        if (which == PreludeFunction::ArrayFilter && callback == booleanConstructor)
            withoutCalls = m_program.preludeFunctions[static_cast<unsigned>(PreludeFunction::ArrayFilterTruthy)];
        else if (which == PreludeFunction::ArrayMap && callback == numberConstructor)
            withoutCalls = m_program.preludeFunctions[static_cast<unsigned>(PreludeFunction::ArrayMapByNumber)];
        return withoutCalls ? withoutCalls : callee;
    }

    std::optional<unsigned> indexOfFusedJoin(BlockID block, unsigned mapIndex) const
    {
        auto& values = m_function.at(block).values;
        ValueID map = values[mapIndex];
        if (m_function.at(map).arguments.size() != 1)
            return std::nullopt;
        for (unsigned index = mapIndex + 1; index < values.size(); ++index) {
            auto& value = m_function.at(values[index]);
            if (value.opcode == Opcode::String)
                continue;
            if (value.opcode != Opcode::CallMethod || !value.isFusedWithMap || value.child(0) != map)
                return std::nullopt;
            if (value.arguments.size() != 1 || m_function.at(value.arguments[0]).opcode != Opcode::String || useCountOf(map) != 1)
                return std::nullopt;
            return index;
        }
        return std::nullopt;
    }

    bool isStringClass(const EngineClass& engineClass) const { return engineClass.index == m_builtins.stringClass().index; }

    const EngineClass* classSpeculatedFor(ValueID receiver, BuiltinName name) const
    {
        auto& type = typeOf(receiver);
        const EngineClass* speculated;
        if (TypeID prediction = m_function.at(receiver).prediction()) {
            Type expected = m_program.at(prediction).withoutPrimitives(Primitive::UndefinedOrNull);
            speculated = expected.isString() ? &m_builtins.stringClass() : expected.singleEngineClass();
            if (!speculated) {
                speculated = m_builtins.classSpeculatedFor(name);
                if (speculated && !(isStringClass(*speculated) ? expected.primitives & Primitive::String : expected.anyObject || expected.engineClasses.contains(speculated->index)))
                    speculated = nullptr;
            }
        } else
            speculated = m_builtins.classSpeculatedFor(name);
        if (!speculated)
            return nullptr;
        if (isStringClass(*speculated))
            return type.primitives & Primitive::String ? speculated : nullptr;
        if (!speculated->canCheckOriginalStructure())
            return nullptr;
        return type.anyObject || type.engineClasses.contains(speculated->index) ? speculated : nullptr;
    }

    bool unboxDateTimes(BlockID block, unsigned index)
    {
        if (m_keepsOriginalDates[block.index()])
            return false;
        ValueID id = m_function.at(block).values[index];
        Vector<ValueID, 2> operands;
        switch (m_function.at(id).opcode) {
        case Opcode::ToNumber:
            operands.append(m_function.at(id).child(0));
            break;
        case Opcode::Call: {
            static const BuiltinName number = Builtins::singleton().nameOf(asBytes("Number"_s.span8()));
            if (m_function.at(id).arguments.size() != 1 || globalNameOf(m_function.at(id).child(0)) != number)
                return false;
            operands.append(m_function.at(id).arguments[0]);
            break;
        }
        case Opcode::Less:
        case Opcode::LessEq:
        case Opcode::Greater:
        case Opcode::GreaterEq:
            operands.append(m_function.at(id).child(0));
            operands.append(m_function.at(id).child(1));
            break;
        default:
            return false;
        }
        const EngineClass* dateClass = nullptr;
        for (ValueID operand : operands) {
            dateClass = typeOf(operand).singleEngineClass();
            if (TypeID prediction = m_function.at(operand).prediction(); !dateClass && prediction && typeOf(operand).anyObject)
                dateClass = m_program.at(prediction).singleEngineClass();
            if (!dateClass || dateClass->type != JSDateType || !dateClass->canCheckOriginalStructure())
                return false;
        }
        BuiltinID entry = applicableEntry(m_builtins.findForInstance(BuiltinKind::Method, *dateClass, m_builtins.nameOf(asBytes("getTime"_s.span8()))), { });
        TypeID typeOfClass = m_program.engineClassTypes[dateClass->index];
        if (!entry || m_builtins.at(entry).implementation.kind == BuiltinImplementation::Generic || !typeOfClass)
            return false;

        Value original = m_function.at(id);
        open(block, index);
        Vector<BlockID, 2> otherwise;
        Vector<ValueID, 2> times;
        for (ValueID operand : operands) {
            Value guard = make(Opcode::CallBuiltin, m_program.types.boolean);
            guard.immediates = { m_builtins.hasOriginalStructureOperation().index(), dateClass->index };
            guard.arguments.append(operand);
            otherwise.append(branch(append(WTF::move(guard))));
            Value call = make(Opcode::CallBuiltin, m_program.types.number);
            call.children[0] = held(operand, typeOfClass);
            call.immediates[0] = entry.index();
            times.append(appendThrowing(WTF::move(call)));
        }
        if (operands.size() == 1)
            finish(held(times[0], original.type));
        else {
            Value comparison = original;
            comparison.children = { times[0], times[1], ValueID() };
            finish(appendThrowing(WTF::move(comparison)));
        }
        for (BlockID slow : otherwise) {
            moveTo(slow);
            m_keepsOriginalDates[slow.index()] = true;
            finish(appendThrowing(Value { original }));
        }
        return true;
    }

    bool readNumberOnEachPath(BlockID block, unsigned index)
    {
        auto& values = m_function.at(block).values;
        ValueID id = values[index];
        auto& read = m_function.at(id);
        if (read.opcode != Opcode::GetById || read.isOnSlowPath || read.hasKnownSlotWord() || read.immediates[1] != Format::none || !read.type || index + 1 == values.size())
            return false;
        switch (auto& next = m_function.at(values[index + 1]); next.opcode) {
        case Opcode::Less:
        case Opcode::LessEq:
        case Opcode::Greater:
        case Opcode::GreaterEq:
            if (next.child(0) != id && next.child(1) != id)
                return false;
            break;
        default:
            return false;
        }
        ValueID receiver = read.child(0);
        TypeID prediction = m_function.at(receiver).prediction();
        if (!prediction || !typeOf(receiver).anyObject)
            return false;
        auto* engineClass = m_program.at(prediction).withoutPrimitives(Primitive::UndefinedOrNull).singleEngineClass();
        if (!engineClass || engineClass->type == ArrayType || !engineClass->canCheckOriginalStructure())
            return false;
        if (auto& refuted = m_refuted[block.index()]; refuted.engineClass == engineClass->index && refuted.receiver == originOf(receiver))
            return false;
        BuiltinName name = m_program.builtinName(read.string());
        BuiltinID entry = name ? m_builtins.findForInstance(BuiltinKind::Getter, *engineClass, name) : BuiltinID();
        if (!entry || m_builtins.at(entry).implementation.kind == BuiltinImplementation::Generic)
            return false;
        TypeID returnedValue = m_program.builtinIntegerResultTypes[entry.index()];
        auto& number = m_program.at(m_program.types.number);
        if (!returnedValue || !m_program.at(returnedValue).isSubsetOf(number, m_program) || read.type == m_program.types.number || !number.isSubsetOf(m_program.at(read.type), m_program))
            return false;
        if (m_isInLoop.isEmpty()) {
            m_isInLoop.fill(false, m_function.blocks.size());
            for (size_t inLoop : blocksInLoops(m_function))
                m_isInLoop[inLoop] = true;
        }
        if (!m_isInLoop[block.index()] || useCountOf(id) != 1)
            return false;

        Value original = read;
        open(block, index);
        Value guard = make(Opcode::CallBuiltin, m_program.types.boolean);
        guard.immediates = { m_builtins.hasOriginalStructureOperation().index(), engineClass->index };
        guard.arguments.append(receiver);
        guard.guardsFastPath = true;
        BlockID slow = branch(append(WTF::move(guard)));
        Value onObject = original;
        onObject.children[0] = held(receiver, m_program.engineClassTypes[engineClass->index]);
        onObject.type = returnedValue;
        onObject.rep = repOf(m_program.at(onObject.type));
        onObject.copyPredictionFrom(Value { });
        finish(appendThrowing(WTF::move(onObject)));

        moveTo(slow);
        m_refuted[slow.index()] = { originOf(receiver), engineClass->index };
        original.isOnSlowPath = true;
        finish(appendThrowing(WTF::move(original)));
        return true;
    }

    struct LiteralValue {
        BlockID block;
        ValueID id;
        bool endsInvokeBlock;
    };

    struct LiteralDefinition {
        StringID name;
        ValueID value;
    };

    struct ObjectLiteral {
        Vector<LiteralDefinition, 6> definitions;
        Vector<LiteralValue, 8> values;
    };

    unsigned useCountInRemainingValues(ValueID id) const
    {
        unsigned uses = 0;
        auto count = [&](ValueID child) {
            uses += child == id;
        };
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            auto& block = m_function.blocks[b];
            for (unsigned i = m_remainingValueCounts[b]; i < block.values.size(); ++i)
                std::as_const(m_function).at(block.values[i]).forEachChild(count);
            const_cast<Terminator&>(block.terminator).forEachChild([&](ValueID& child) {
                count(child);
            });
        }
        return uses;
    }

    BlockID fallThroughPredecessor(BlockID block) const
    {
        BlockID found;
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            auto& terminator = m_function.blocks[b].terminator;
            for (unsigned target = 0; target < terminator.targets.size(); ++target) {
                if (terminator.targets[target].block != block)
                    continue;
                if (found || target || (terminator.kind != TerminatorKind::Jump && terminator.kind != TerminatorKind::Invoke))
                    return { };
                found = BlockID(b);
            }
        }
        return found;
    }

    std::optional<LiteralDefinition> definitionIn(ValueID object, const Value& value) const
    {
        if (value.opcode == Opcode::PutById && value.child(0) == object && value.child(1) != object && !value.isInitialStore())
            return LiteralDefinition { value.string(), value.child(1) };
        if (value.opcode == Opcode::PutField && value.child(0) == object && value.child(1) != object && m_function.at(object).opcode == Opcode::NewObjectWithShape && value.layout() == m_function.at(object).layout())
            return LiteralDefinition { m_program.at(value.layout()).fields[value.field()].name, value.child(1) };
        if (languageOperationOf(m_program, value) != LanguageOperation::DefineField || value.arguments.size() != 3 || value.arguments[0] != object || value.arguments[2] == object || m_function.at(value.arguments[1]).opcode != Opcode::String)
            return std::nullopt;
        return LiteralDefinition { m_function.at(value.arguments[1]).string(), value.arguments[2] };
    }

    std::optional<ObjectLiteral> literalUsedOnlyBy(ValueID object, BlockID block, size_t user) const
    {
        constexpr unsigned maxBlocks = 32;
        if (auto& made = m_function.at(object); made.opcode != Opcode::NewObject && (made.opcode != Opcode::NewObjectWithShape || m_program.at(made.layout()).kind != LayoutKind::Record))
            return std::nullopt;
        BlockID handler = m_function.at(block).terminator.kind == TerminatorKind::Invoke ? m_function.at(block).terminator.targets[1].block : BlockID();
        ObjectLiteral literal;
        for (unsigned visited = 0; visited < maxBlocks; ++visited) {
            auto& current = m_function.at(block);
            for (size_t i = user; i-- > m_remainingValueCounts[block.index()];) {
                ValueID id = current.values[i];
                bool endsInvokeBlock = current.terminator.kind == TerminatorKind::Invoke && i + 1 == current.values.size();
                if (id == object) {
                    literal.values.append({ block, id, false });
                    literal.values.reverse();
                    literal.definitions.reverse();
                    if (useCountInRemainingValues(object) != literal.definitions.size() + 1)
                        return std::nullopt;
                    return literal;
                }
                auto& value = m_function.at(id);
                if (auto definition = definitionIn(object, value)) {
                    auto name = m_program.string(definition->name);
                    bool isPlainName = !name.empty() && !isASCIIDigit(name[0]) && !equalSpans(name, asBytes("__proto__"_s.span8()));
                    bool isRepeated = literal.definitions.containsIf([&](auto& later) { return later.name == definition->name; });
                    if (!isPlainName || isRepeated || (endsInvokeBlock && current.terminator.targets[1].block != handler))
                        return std::nullopt;
                    literal.definitions.append(*definition);
                    literal.values.append({ block, id, endsInvokeBlock });
                    continue;
                }
                bool usesObject = false;
                value.forEachChild([&](ValueID child) {
                    usesObject |= child == object;
                });
                if (usesObject)
                    return std::nullopt;
            }
            block = fallThroughPredecessor(block);
            if (!block)
                return std::nullopt;
            user = m_function.at(block).values.size();
        }
        return std::nullopt;
    }

    struct ResponseInit {
        ValueID status;
        ValueID statusText;
        ValueID headers;
        Vector<LiteralDefinition, 6> headerDefinitions;
        Vector<LiteralValue, 16> literalValues;
    };

    std::optional<ResponseInit> responseInitLiteral(ValueID object, BlockID block, size_t user) const
    {
        auto init = literalUsedOnlyBy(object, block, user);
        if (!init)
            return std::nullopt;
        auto isNamed = [&](StringID name, ASCIILiteral text) {
            return equalSpans(m_program.string(name), asBytes(text.span8()));
        };
        ResponseInit result;
        for (size_t i = 0; i < init->definitions.size(); ++i) {
            auto& definition = init->definitions[i];
            if (isNamed(definition.name, "status"_s))
                result.status = definition.value;
            else if (isNamed(definition.name, "statusText"_s))
                result.statusText = definition.value;
            else if (isNamed(definition.name, "headers"_s)) {
                auto& store = init->values[i + 1];
                size_t index = m_function.at(store.block).values.reverseFind(store.id);
                auto headers = literalUsedOnlyBy(definition.value, store.block, index);
                if (headers && std::ranges::all_of(headers->definitions, [&](auto& header) { return knownHeaderName(header.name) != unknownHeaderName; })) {
                    result.headerDefinitions = WTF::move(headers->definitions);
                    result.literalValues.appendVector(headers->values);
                } else
                    result.headers = definition.value;
            } else
                return std::nullopt;
        }
        result.literalValues.appendVector(init->values);
        return result;
    }

    void removeFromBlocks(const ResponseInit& init)
    {
        for (auto& moved : init.literalValues) {
            auto& block = m_function.at(moved.block);
            block.values.removeLast(moved.id);
            if (!moved.endsInvokeBlock)
                continue;
            block.terminator.kind = TerminatorKind::Jump;
            block.terminator.targets.shrink(1);
        }
    }

    void appendToSlowPath(const ResponseInit& init)
    {
        for (auto& moved : init.literalValues) {
            endBlockAfterThrowingValue();
            m_function.at(m_block).values.append(moved.id);
            m_lastMayThrow = m_hasHandler && moved.endsInvokeBlock;
        }
    }

    Value callWithResponseInit(BuiltinID operation, TypeID type, ValueID first, const ResponseInit& init)
    {
        Value call = make(Opcode::CallBuiltin, type);
        call.immediates[0] = operation.index();
        call.arguments.append(first);
        for (ValueID operand : { init.status, init.statusText, init.headers })
            call.arguments.append(operand ? operand : append(make(Opcode::Undefined, m_program.types.undefined)));
        for (auto& header : init.headerDefinitions) {
            uint32_t number = knownHeaderName(header.name);
            Value name = make(Opcode::String, m_program.types.string);
            name.immediates[0] = header.name.index();
            if (number != uncommonHeaderName) {
                name = make(Opcode::Integer, m_program.types.int32);
                name.setBits(number);
            }
            call.arguments.append(append(WTF::move(name)));
            call.arguments.append(header.value);
        }
        return call;
    }

    bool isBehindFailedGuardOf(ValueID callee, BlockID block) const
    {
        constexpr unsigned maxBlocks = 32;
        for (unsigned visited = 0; block && visited < maxBlocks; ++visited) {
            if (m_calleesThatFailedGuard[block.index()] == callee)
                return true;
            block = fallThroughPredecessor(block);
        }
        return !!block;
    }

    struct GetterRead {
        ValueID id;
        BlockID block;
        ValueID receiver;
        size_t firstValueBehind;
    };

    bool readsReplaceableGlobalThroughCache(ValueID id) const
    {
        auto& value = m_function.at(id);
        auto& source = value.inlineFrame ? m_program.at(m_function.inlineFrames[value.inlineFrame].callee) : m_function;
        return value.opcode == Opcode::GetGlobal && !source.has(Format::FunctionIsInBuiltinModule);
    }

    bool writesNothingBetween(BlockID block, size_t begin, size_t end, ValueID callee) const
    {
        auto& values = m_function.at(block).values;
        for (size_t i = begin; i < end; ++i) {
            if (values[i] == callee && readsReplaceableGlobalThroughCache(callee))
                continue;
            auto& value = m_function.at(values[i]);
            if (value.opcode == Opcode::CallDirect || value.opcode == Opcode::Await || value.opcode == Opcode::Yield || value.opcode == Opcode::GetResumeMode)
                return false;
            if (value.opcode == Opcode::Operation || value.opcode == Opcode::CallBuiltin) {
                BuiltinID operation = m_program.operationOf(value);
                if (operation == Operations::HostObjectCachedValue || operation == Operations::HasHostObjectCachedValue)
                    return false;
            }
            Writes writes = value.opcode == Opcode::Operation ? writesOfResolvedBuiltinCall(m_program, m_function, value) : writesOf(m_program, m_function, m_function.at(block), i);
            if (writes > Writes::RuntimeState)
                return false;
        }
        return true;
    }

    bool areUsedOnlyUpTo(BlockID block, size_t begin, size_t last) const
    {
        auto& values = m_function.at(block).values;
        for (size_t i = begin; i < last; ++i) {
            unsigned uses = 0;
            for (size_t user = i + 1; user <= last; ++user) {
                std::as_const(m_function).at(values[user]).forEachChild([&](ValueID child) {
                    uses += child == values[i];
                });
            }
            if (uses != useCountInRemainingValues(values[i]))
                return false;
        }
        return true;
    }

    bool isReadUsedOnce(ValueID id, ASCIILiteral name) const
    {
        auto& value = m_function.at(id);
        return value.opcode == Opcode::GetById && equalSpans(m_program.string(value.string()), asBytes(name.span8())) && useCountInRemainingValues(id) == 1;
    }

    bool isUncheckedCastUsedOnce(ValueID id) const
    {
        auto& value = m_function.at(id);
        if (value.opcode != Opcode::Cast || useCountInRemainingValues(id) != 1)
            return false;
        auto reason = static_cast<CastReason>(value.immediates[1]);
        return reason == CastReason::Claim || reason == CastReason::Upcast;
    }

    std::optional<GetterRead> getterReadUsedOnlyBy(ASCIILiteral name, ValueID operand, BlockID block, size_t user, ValueID callee) const
    {
        size_t first = m_remainingValueCounts[block.index()];
        if (m_function.at(operand).opcode == Opcode::Cast) {
            if (!isUncheckedCastUsedOnce(operand))
                return std::nullopt;
            operand = m_function.at(operand).child(0);
        }
        if (isReadUsedOnce(operand, name)) {
            size_t index = m_function.at(block).values.reverseFind(operand);
            if (index == notFound || index < first || index >= user || !writesNothingBetween(block, index + 1, user, callee))
                return std::nullopt;
            return GetterRead { operand, block, m_function.at(operand).child(0), index + 1 };
        }
        auto parameters = m_function.at(block).parameters.span();
        size_t position = std::ranges::find(parameters, operand) - parameters.begin();
        if (m_function.at(operand).opcode != Opcode::Parameter || position == parameters.size() || useCountInRemainingValues(operand) != 1 || !writesNothingBetween(block, first, user, callee))
            return std::nullopt;
        std::optional<GetterRead> read;
        ValueID cached;
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            auto& predecessor = m_function.blocks[b];
            for (auto& target : predecessor.terminator.targets) {
                if (target.block != block)
                    continue;
                ValueID incoming = target.arguments[position];
                if (predecessor.terminator.kind != TerminatorKind::Jump)
                    return std::nullopt;
                if (isReadUsedOnce(incoming, name)) {
                    if (read || predecessor.values.isEmpty() || predecessor.values.last() != incoming || predecessor.values.size() <= m_remainingValueCounts[b])
                        return std::nullopt;
                    read = GetterRead { incoming, BlockID(b), m_function.at(incoming).child(0), first };
                    continue;
                }
                auto& other = m_function.at(incoming);
                bool readsCache = (other.opcode == Opcode::Operation || other.opcode == Opcode::CallBuiltin) && m_program.operationOf(other) == Operations::HostObjectCachedValue;
                if (cached || !readsCache)
                    return std::nullopt;
                cached = incoming;
            }
        }
        if (!read || !cached || m_function.at(cached).arguments[0] != read->receiver)
            return std::nullopt;
        return read;
    }

    ValueID appendTestWithNumber(BuiltinID operation, ValueID subject, uint32_t number)
    {
        Value constant = make(Opcode::Integer, m_program.types.int32);
        constant.setBits(number);
        Value test = make(Opcode::CallBuiltin, m_program.types.boolean);
        test.immediates[0] = operation.index();
        if (subject)
            test.arguments.append(subject);
        test.arguments.append(append(WTF::move(constant)));
        return append(WTF::move(test));
    }

    void branchOrElse(ValueID condition, BlockID otherwise)
    {
        endBlockAfterThrowingValue();
        BlockID then = addBlock();
        auto& branch = m_function.at(m_block).terminator;
        branch.kind = TerminatorKind::Branch;
        branch.value = condition;
        branch.targets.append(Target { then, { } });
        branch.targets.append(Target { otherwise, { } });
        moveTo(then);
    }

    bool specializeResponseFromResponse(BlockID block, unsigned index, BuiltinName global)
    {
        Value original = m_function.at(m_function.at(block).values[index]);
        ValueID callee = original.child(0);
        if (m_function.at(block).terminator.kind == TerminatorKind::Invoke)
            return false;
        auto read = getterReadUsedOnlyBy("body"_s, original.arguments[0], block, index, callee);
        if (!read || !areUsedOnlyUpTo(block, read->firstValueBehind, index))
            return false;
        ValueID receiver = read->receiver;
        Value bodyRead = m_function.at(read->id);
        std::optional<Value> cast;
        if (m_function.at(original.arguments[0]).opcode == Opcode::Cast)
            cast = m_function.at(original.arguments[0]);
        Vector<ValueID, 8> repeated;
        for (size_t i = read->firstValueBehind; i < index; ++i) {
            if (ValueID id = m_function.at(block).values[i]; id != original.arguments[0])
                repeated.append(id);
        }
        bool calleeIsReadBehind = repeated.contains(callee);

        open(block, index);
        BlockID afterConstruction = m_after;
        uint32_t position = m_position;
        uint32_t inlineFrame = m_inlineFrame;
        uint32_t inliningDepth = m_inliningDepth;
        bool isInTailPosition = m_isInTailPosition;
        BlockID asWritten = addBlock();
        m_calleesThatFailedGuard[asWritten.index()] = callee;
        m_function.at(m_block).terminator.kind = TerminatorKind::Jump;
        m_function.at(m_block).terminator.targets.append(Target { asWritten, { } });
        moveTo(asWritten);
        finish(appendThrowing(Value(original)));

        open(read->block, static_cast<unsigned>(m_function.at(read->block).values.reverseFind(read->id)));
        BlockID afterRead = m_after;
        BlockID readsNow = addBlock();
        branchOrElse(appendTestWithNumber(Operations::HasHostClassStructure, receiver, responseHostClass), readsNow);
        if (calleeIsReadBehind)
            branchOrElse(appendTestWithNumber(Operations::IsGlobalCachedAsBuiltin, { }, global.index()), readsNow);
        else {
            Value guard = make(Opcode::IsBuiltinFunction, m_program.types.boolean);
            guard.children[0] = callee;
            guard.immediates[1] = global.index();
            branchOrElse(append(WTF::move(guard)), readsNow);
        }

        Vector<std::pair<ValueID, ValueID>, 8> copies;
        auto copyOf = [&](ValueID id) {
            for (auto& [from, to] : copies) {
                if (from == id)
                    return to;
            }
            return id;
        };
        for (ValueID id : repeated) {
            Value copy = m_function.at(id);
            copy.forEachChild([&](ValueID& child) {
                child = copyOf(child);
            });
            copies.append({ id, append(WTF::move(copy)) });
        }
        m_after = afterConstruction;
        m_position = position;
        m_inlineFrame = inlineFrame;
        m_inliningDepth = inliningDepth;
        m_isInTailPosition = isInTailPosition;
        Value claim = make(Opcode::Cast, m_program.types.object);
        claim.children[0] = receiver;
        claim.immediates[1] = static_cast<uint32_t>(CastReason::Claim);
        ValueID response = append(WTF::move(claim));
        Value isSame = make(Opcode::StrictEq, m_program.types.boolean);
        isSame.children = { copyOf(original.arguments[1]), response };
        BlockID readsLate = addBlock();
        m_calleesThatFailedGuard[readsLate.index()] = copyOf(callee);
        branchOrElse(append(WTF::move(isSame)), readsLate);
        Value call = make(Opcode::CallBuiltin, original.type);
        call.immediates[0] = Operations::NewResponseFromResponse.index();
        call.arguments.append(response);
        finish(appendThrowing(WTF::move(call)));

        moveTo(readsLate);
        ValueID body = appendThrowing(Value(bodyRead));
        if (cast) {
            cast->children[0] = body;
            body = appendThrowing(WTF::move(*cast));
        }
        original.children[0] = copyOf(callee);
        original.arguments[0] = body;
        original.arguments[1] = copyOf(original.arguments[1]);
        finish(appendThrowing(WTF::move(original)));

        m_after = afterRead;
        moveTo(readsNow);
        finish(appendThrowing(WTF::move(bodyRead)));
        return true;
    }

    enum class HeadersSubject : uint8_t { Holder, HeadersObject };

    void branchUnlessHoldsOriginalHeaders(ValueID subject, BlockID otherwise, HeadersSubject kind = HeadersSubject::Holder)
    {
        static constexpr unsigned holders[] = { responseHostClass, requestHostClass, bunRequestHostClass };
        static constexpr unsigned headers[] = { headersHostClass };
        std::span<const unsigned> classes = kind == HeadersSubject::Holder ? std::span<const unsigned>(holders) : std::span<const unsigned>(headers);
        BlockID then = addBlock();
        for (unsigned i = 0; i < classes.size(); ++i) {
            ValueID isInstance = appendTestWithNumber(Operations::HasHostClassStructure, subject, classes[i]);
            endBlockAfterThrowingValue();
            BlockID next = i + 1 < classes.size() ? addBlock() : otherwise;
            auto& terminator = m_function.at(m_block).terminator;
            terminator.kind = TerminatorKind::Branch;
            terminator.value = isInstance;
            terminator.targets.append(Target { then, { } });
            terminator.targets.append(Target { next, { } });
            moveTo(next);
        }
        moveTo(then);
    }

    uint32_t knownHeaderName(ValueID id) const
    {
        auto& value = m_function.at(id);
        return value.opcode == Opcode::String ? knownHeaderName(value.string()) : unknownHeaderName;
    }

    uint32_t knownHeaderName(StringID name) const { return knownHeaderNameOf(m_program.string(name)); }

    bool specializeHeaderRead(BlockID block, unsigned index)
    {
        Value original = m_function.at(m_function.at(block).values[index]);
        if (original.opcode != Opcode::CallMethod || original.arguments.size() != 1 || !equalSpans(m_program.string(original.string()), asBytes("get"_s.span8())))
            return false;
        ValueID headers = original.child(0);
        if (m_function.at(block).terminator.kind == TerminatorKind::Invoke || isBehindFailedGuardOf(headers, block))
            return false;
        auto read = getterReadUsedOnlyBy("headers"_s, headers, block, index, { });
        if (!read || !areUsedOnlyUpTo(block, read->firstValueBehind, index))
            return false;
        ValueID holder = read->receiver;
        Value headersRead = m_function.at(read->id);
        Vector<ValueID, 8> repeated;
        for (size_t i = read->firstValueBehind; i < index; ++i) {
            if (ValueID id = m_function.at(block).values[i]; id != headers)
                repeated.append(id);
        }

        open(block, index);
        BlockID afterCall = m_after;
        uint32_t position = m_position;
        uint32_t inlineFrame = m_inlineFrame;
        uint32_t inliningDepth = m_inliningDepth;
        bool isInTailPosition = m_isInTailPosition;
        BlockID asWritten = addBlock();
        m_calleesThatFailedGuard[asWritten.index()] = headers;
        m_function.at(m_block).terminator.kind = TerminatorKind::Jump;
        m_function.at(m_block).terminator.targets.append(Target { asWritten, { } });
        moveTo(asWritten);
        finish(appendThrowing(Value(original)));

        open(read->block, static_cast<unsigned>(m_function.at(read->block).values.reverseFind(read->id)));
        BlockID afterRead = m_after;
        BlockID readsNow = addBlock();
        branchUnlessHoldsOriginalHeaders(holder, readsNow);
        Vector<std::pair<ValueID, ValueID>, 8> copies;
        auto copyOf = [&](ValueID id) {
            for (auto& [from, to] : copies) {
                if (from == id)
                    return to;
            }
            return id;
        };
        for (ValueID id : repeated) {
            Value copy = m_function.at(id);
            copy.forEachChild([&](ValueID& child) {
                child = copyOf(child);
            });
            copies.append({ id, append(WTF::move(copy)) });
        }
        m_after = afterCall;
        m_position = position;
        m_inlineFrame = inlineFrame;
        m_inliningDepth = inliningDepth;
        m_isInTailPosition = isInTailPosition;
        Value call = make(Opcode::CallBuiltin, original.type);
        call.immediates[0] = Operations::GetRequestOrResponseHeader.index();
        call.arguments.append(holder);
        call.arguments.append(copyOf(original.arguments[0]));
        Value knownName = make(Opcode::Integer, m_program.types.int32);
        knownName.setBits(knownHeaderName(original.arguments[0]));
        call.arguments.append(append(WTF::move(knownName)));
        ValueID header = appendThrowing(WTF::move(call));
        Value isUnanswered = make(Opcode::StrictEq, m_program.types.boolean);
        isUnanswered.children = { header, append(make(Opcode::Undefined, m_program.types.undefined)) };
        BlockID answered = branch(append(WTF::move(isUnanswered)));
        original.children[0] = appendThrowing(Value(headersRead));
        m_calleesThatFailedGuard[m_block.index()] = original.children[0];
        original.arguments[0] = copyOf(original.arguments[0]);
        finish(appendThrowing(WTF::move(original)));

        moveTo(answered);
        finish(header);

        m_after = afterRead;
        moveTo(readsNow);
        finish(appendThrowing(WTF::move(headersRead)));
        return true;
    }

    std::optional<HeadersMethod> headersMethodNamed(StringID string, unsigned argumentCount) const
    {
        auto isNamed = [&](ASCIILiteral name) {
            return equalSpans(m_program.string(string), asBytes(name.span8()));
        };
        if (argumentCount == 1) {
            if (isNamed("get"_s))
                return HeadersMethod::Get;
            if (isNamed("has"_s))
                return HeadersMethod::Has;
            if (isNamed("delete"_s))
                return HeadersMethod::Delete;
        }
        if (argumentCount == 2) {
            if (isNamed("set"_s))
                return HeadersMethod::Set;
            if (isNamed("append"_s))
                return HeadersMethod::Append;
        }
        return std::nullopt;
    }

    ValueID addParameter(BlockID block, TypeID type)
    {
        Value parameter = make(Opcode::Parameter, type);
        parameter.immediates = { block.index(), static_cast<uint32_t>(m_function.at(block).parameters.size()) };
        m_function.values.append(WTF::move(parameter));
        ValueID id(m_function.values.size() - 1);
        m_function.at(block).parameters.append(id);
        return id;
    }

    void jump(BlockID block, ValueID argument)
    {
        auto& terminator = m_function.at(m_block).terminator;
        terminator.kind = m_lastMayThrow ? TerminatorKind::Invoke : TerminatorKind::Jump;
        terminator.targets.append(Target { block, Arguments { argument } });
        if (m_lastMayThrow)
            terminator.targets.append(m_handler);
        m_lastMayThrow = false;
    }

    std::pair<BlockID, unsigned> placeOf(ValueID id) const
    {
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            auto& values = m_function.blocks[b].values;
            for (unsigned i = m_remainingValueCounts[b]; i < values.size(); ++i) {
                if (values[i] == id)
                    return { BlockID(b), i };
            }
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    ValueID appendCachedHeaders(ValueID holder)
    {
        Value offset = make(Opcode::Integer, m_program.types.int32);
        offset.setBits(offsetOfCachedHeaders);
        Value read = make(Opcode::CallBuiltin, m_program.types.anything);
        read.isInTailPosition = false;
        read.immediates[0] = Operations::HostObjectCachedValue.index();
        read.arguments.append(holder);
        read.arguments.append(append(WTF::move(offset)));
        return append(WTF::move(read));
    }

    void finish(ValueID result, std::initializer_list<ValueID> more)
    {
        finish(result);
        for (ValueID argument : more)
            m_function.at(m_block).terminator.targets[0].arguments.append(argument);
    }

    ValueID keepHeadersInHolder(ValueID read, ValueID yes, ValueID no)
    {
        auto [block, index] = placeOf(read);
        m_siteBlock = block;
        Value original = m_function.at(read);
        ValueID holder = original.child(0);
        open(block, index);
        ValueID standsForHeaders = addParameter(m_after, m_program.types.boolean);
        BlockID readsNow = addBlock();
        branchUnlessHoldsOriginalHeaders(holder, readsNow);
        BlockID isNotCreated = branch(appendTestWithNumber(Operations::HostObjectHasCachedValue, holder, offsetOfCachedHeaders));
        finish(held(appendCachedHeaders(holder), original.type), { no });

        moveTo(isNotCreated);
        finish(held(holder, original.type), { yes });

        moveTo(readsNow);
        finish(appendThrowing(WTF::move(original)), { no });
        return standsForHeaders;
    }

    void branchOnHeaders(ValueID value, ValueID standsForHeaders, BlockID hasOriginalMethods, BlockID otherwise)
    {
        BlockID isObject = addBlock();
        ValueID object = addParameter(isObject, m_program.types.anything);
        if (m_function.at(standsForHeaders).opcode == Opcode::Boolean)
            jump(isObject, value);
        else {
            BlockID isHolder = addBlock();
            BlockID isNoHolder = addBlock();
            auto& test = m_function.at(m_block).terminator;
            test.kind = TerminatorKind::Branch;
            test.value = standsForHeaders;
            test.targets.append(Target { isHolder, { } });
            test.targets.append(Target { isNoHolder, { } });

            moveTo(isNoHolder);
            jump(isObject, value);

            moveTo(isHolder);
            BlockID isNotCreated = branch(appendTestWithNumber(Operations::HostObjectHasCachedValue, value, offsetOfCachedHeaders));
            jump(isObject, appendCachedHeaders(value));

            moveTo(isNotCreated);
            jump(hasOriginalMethods, value);
        }

        moveTo(isObject);
        BlockID isChanged = addBlock();
        branchUnlessHoldsOriginalHeaders(object, isChanged, HeadersSubject::HeadersObject);
        jump(hasOriginalMethods, object);

        moveTo(isChanged);
        jump(otherwise, object);
    }

    ValueID appendHeadersMethodCall(HeadersMethod method, ValueID headersOrHolder, const Value& original)
    {
        Value call = make(Opcode::CallBuiltin, m_program.types.anything);
        call.isInTailPosition = false;
        call.immediates[0] = Operations::CallHeadersMethod.index();
        call.arguments.append(headersOrHolder);
        call.arguments.append(original.arguments[0]);
        call.arguments.append(original.arguments.size() == 2 ? original.arguments[1] : append(make(Opcode::Undefined, m_program.types.undefined)));
        Value methodAndKnownName = make(Opcode::Integer, m_program.types.int32);
        methodAndKnownName.setBits(static_cast<uint32_t>(method) << headersMethodShift | knownHeaderName(original.arguments[0]));
        call.arguments.append(append(WTF::move(methodAndKnownName)));
        return held(appendThrowing(WTF::move(call)), original.type);
    }

    void callHeadersMethod(ValueID site, HeadersMethod method, ValueID standsForHeaders)
    {
        auto [block, index] = placeOf(site);
        m_siteBlock = block;
        Value original = m_function.at(site);
        open(block, index);
        BlockID calls = addBlock();
        ValueID headersOrHolder = addParameter(calls, m_program.types.anything);
        BlockID asWritten = addBlock();
        ValueID object = addParameter(asWritten, m_program.types.anything);
        branchOnHeaders(original.child(0), standsForHeaders, calls, asWritten);

        moveTo(calls);
        finish(appendHeadersMethodCall(method, headersOrHolder, original));

        moveTo(asWritten);
        original.children[0] = object;
        finish(appendThrowing(WTF::move(original)));
    }

    void callHeadersMethodReadEarlier(ValueID methodGetById, ValueID site, HeadersMethod method, ValueID standsForHeaders, ValueID yes, ValueID no)
    {
        auto [readBlock, readIndex] = placeOf(methodGetById);
        m_siteBlock = readBlock;
        Value read = m_function.at(methodGetById);
        open(readBlock, readIndex);
        m_function.at(methodGetById).type = m_program.types.anything;
        m_function.at(methodGetById).rep = repOf(m_program.at(m_program.types.anything));
        ValueID receiver = addParameter(m_after, m_program.types.anything);
        ValueID hasOriginalMethod = addParameter(m_after, m_program.types.boolean);
        BlockID isOriginal = addBlock();
        ValueID headersOrHolder = addParameter(isOriginal, m_program.types.anything);
        BlockID asWritten = addBlock();
        ValueID object = addParameter(asWritten, m_program.types.anything);
        branchOnHeaders(read.child(0), standsForHeaders, isOriginal, asWritten);

        moveTo(isOriginal);
        finish(append(make(Opcode::Undefined, m_program.types.undefined)), { headersOrHolder, yes });

        moveTo(asWritten);
        read.children[0] = object;
        finish(appendThrowing(WTF::move(read)), { object, no });

        auto [block, index] = placeOf(site);
        m_siteBlock = block;
        Value original = m_function.at(site);
        open(block, index);
        BlockID calls = branchingOn(hasOriginalMethod);
        original.children[1] = receiver;
        Value call = original;
        finish(appendThrowing(WTF::move(call)));

        moveTo(calls);
        finish(appendHeadersMethodCall(method, receiver, original));
    }

    BlockID branchingOn(ValueID condition)
    {
        BlockID then = addBlock();
        BlockID otherwise = addBlock();
        auto& test = m_function.at(m_block).terminator;
        test.kind = TerminatorKind::Branch;
        test.value = condition;
        test.targets.append(Target { then, { } });
        test.targets.append(Target { otherwise, { } });
        moveTo(otherwise);
        return then;
    }

    bool isMethodReadOfOneCall(ValueID id) const
    {
        auto& read = m_function.at(id);
        if (read.opcode != Opcode::GetById || read.isOnSlowPath || read.hasKnownSlotWord())
            return false;
        unsigned calls = 0;
        unsigned uses = 0;
        for (auto& block : m_function.blocks) {
            for (ValueID user : block.values) {
                auto& value = m_function.at(user);
                value.forEachChild([&](ValueID child) {
                    uses += child == id;
                });
                calls += value.opcode == Opcode::Call && value.child(0) == id && value.child(1) == read.child(0) && !!headersMethodNamed(read.string(), value.arguments.size());
            }
            const_cast<Terminator&>(block.terminator).forEachChild([&](ValueID& child) {
                uses += child == id;
            });
        }
        return calls == 1 && uses == 1;
    }

    unsigned keepHeadersInHolders()
    {
        constexpr unsigned none = UINT_MAX;
        Vector<ValueID, 8> members;
        Vector<unsigned, 8> leaders;
        unsigned numberOfReads = 0;
        auto indexOf = [&](ValueID id) {
            size_t index = members.find(id);
            return index == notFound ? none : static_cast<unsigned>(index);
        };
        auto leaderOf = [&](unsigned index) {
            while (leaders[index] != index)
                index = leaders[index];
            return index;
        };
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values) {
                auto& value = m_function.at(id);
                if (value.opcode != Opcode::GetById || value.isOnSlowPath || value.hasKnownSlotWord() || !equalSpans(m_program.string(value.string()), asBytes("headers"_s.span8())))
                    continue;
                if (!m_program.at(value.type).anyObject || !typeOf(value.child(0)).anyObject)
                    continue;
                leaders.append(members.size());
                members.append(id);
            }
        }
        numberOfReads = members.size();
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values) {
                auto& value = m_function.at(id);
                if (value.opcode != Opcode::Construct || !m_program.at(value.type).anyObject)
                    continue;
                auto& callee = m_function.at(originOf(value.child(0)));
                if (callee.opcode != Opcode::GetGlobal || !equalSpans(m_program.string(callee.string()), asBytes("Headers"_s.span8())))
                    continue;
                leaders.append(members.size());
                members.append(id);
            }
        }
        unsigned numberOfSeeds = members.size();
        if (!numberOfSeeds)
            return 0;
        auto isConstructed = [&](unsigned index) {
            return index >= numberOfReads && index < numberOfSeeds;
        };

        constexpr unsigned maxMembers = 64;
        auto join = [&](ValueID from, ValueID to) {
            unsigned source = indexOf(from);
            if (source == none)
                return false;
            unsigned target = indexOf(to);
            if (target == none) {
                target = members.size();
                leaders.append(target);
                members.append(to);
            }
            if (leaderOf(source) == leaderOf(target))
                return false;
            leaders[leaderOf(target)] = leaderOf(source);
            return true;
        };
        for (bool changed = true; changed && members.size() <= maxMembers;) {
            changed = false;
            for (auto& block : m_function.blocks) {
                for (ValueID id : block.values) {
                    auto& value = m_function.at(id);
                    if (value.opcode == Opcode::Cast && (value.castReason() == CastReason::Claim || value.castReason() == CastReason::Upcast) && m_program.at(value.type).anyObject)
                        changed |= join(value.child(0), id);
                }
                for (auto& target : block.terminator.targets) {
                    auto parameters = m_function.at(target.block).parameters.span();
                    for (unsigned k = 0; k < target.arguments.size(); ++k)
                        changed |= join(target.arguments[k], parameters[k]);
                }
            }
        }
        if (members.size() > maxMembers)
            return 0;

        struct Site {
            ValueID id;
            HeadersMethod method;
            unsigned group;
            ValueID methodGetById;
        };
        Vector<Site, 8> sites;
        Vector<bool, 8> isRefused(FillWith { }, members.size(), false);
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values) {
                auto& value = m_function.at(id);
                if (indexOf(id) != none && value.opcode == Opcode::Cast)
                    continue;
                if (value.opcode == Opcode::GetById && indexOf(value.child(0)) != none && isMethodReadOfOneCall(id))
                    continue;
                std::optional<HeadersMethod> method;
                ValueID receiver;
                ValueID methodGetById;
                if (value.opcode == Opcode::CallMethod) {
                    method = headersMethodNamed(value.string(), value.arguments.size());
                    receiver = value.child(0);
                } else if (value.opcode == Opcode::Call && m_function.at(value.child(0)).opcode == Opcode::GetById && m_function.at(value.child(0)).child(0) == value.child(1) && isMethodReadOfOneCall(value.child(0))) {
                    method = headersMethodNamed(m_function.at(value.child(0)).string(), value.arguments.size());
                    receiver = value.child(1);
                    methodGetById = value.child(0);
                }
                unsigned uses = 0;
                value.forEachChild([&](ValueID child) {
                    unsigned index = indexOf(child);
                    if (index == none || isConstructed(index))
                        return;
                    if (!method || child != receiver || ++uses > 1)
                        isRefused[leaderOf(index)] = true;
                });
                if (unsigned index = method ? indexOf(receiver) : none; index != none)
                    sites.append({ id, *method, leaderOf(index), methodGetById });
            }
            if (unsigned index = block.terminator.value ? indexOf(block.terminator.value) : none; index != none && !isConstructed(index))
                isRefused[leaderOf(index)] = true;
        }
        for (unsigned index = 0; index < members.size(); ++index) {
            if (!m_program.at(m_function.at(members[index]).type).anyObject)
                isRefused[leaderOf(index)] = true;
        }
        Vector<bool, 8> hasRead(FillWith { }, members.size(), false);
        for (unsigned index = 0; index < numberOfReads; ++index)
            hasRead[leaderOf(index)] = true;
        auto hasMoreThanALoneGet = [&](unsigned group) {
            unsigned count = 0;
            bool onlyReads = true;
            for (auto& site : sites) {
                if (site.group != group)
                    continue;
                ++count;
                onlyReads &= site.method == HeadersMethod::Get;
            }
            return count > 1 || (count && !onlyReads);
        };
        Vector<bool, 8> taken(members.size(), [&](size_t index) {
            unsigned group = leaderOf(index);
            return !isRefused[group] && hasRead[group] && hasMoreThanALoneGet(group);
        });
        auto isCalledOnObject = [&](const Site& site) {
            if (taken[site.group] || (hasRead[site.group] && !hasMoreThanALoneGet(site.group)))
                return false;
            ValueID name = m_function.at(site.id).arguments[0];
            return knownHeaderName(name) != unknownHeaderName || m_function.blocks[0].parameters.contains(originOf(name));
        };
        if (!taken.contains(true) && !sites.containsIf(isCalledOnObject))
            return 0;

        m_position = 0;
        m_inlineFrame = 0;
        m_isInTailPosition = false;
        auto constant = [&](bool truth) {
            Value value = make(Opcode::Boolean, m_program.types.boolean);
            value.immediates[0] = truth;
            m_function.values.append(WTF::move(value));
            ValueID id(m_function.values.size() - 1);
            m_function.blocks[0].values.insert(0, id);
            return id;
        };
        ValueID yes = constant(true);
        ValueID no = constant(false);

        Vector<ValueID, 8> flags(FillWith { }, members.size(), ValueID());
        Vector<BlockID, 8> joins;
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            unsigned count = m_function.blocks[b].parameters.size();
            for (unsigned k = 0; k < count; ++k) {
                unsigned index = indexOf(m_function.blocks[b].parameters[k]);
                if (index == none || !taken[index])
                    continue;
                flags[index] = addParameter(BlockID(b), m_program.types.boolean);
                if (!joins.contains(BlockID(b)))
                    joins.append(BlockID(b));
            }
        }
        for (unsigned index = 0; index < numberOfReads; ++index) {
            if (taken[index])
                flags[index] = keepHeadersInHolder(members[index], yes, no);
        }
        auto flagOf = [&](ValueID id) {
            for (;;) {
                unsigned index = indexOf(id);
                if (index == none || !taken[index])
                    return no;
                if (flags[index])
                    return flags[index];
                id = m_function.at(id).child(0);
            }
        };
        for (auto& block : m_function.blocks) {
            for (auto& target : block.terminator.targets) {
                if (!joins.contains(target.block))
                    continue;
                auto parameters = m_function.at(target.block).parameters.span();
                unsigned count = target.arguments.size();
                for (unsigned k = 0; k < count; ++k) {
                    if (unsigned index = indexOf(parameters[k]); index != none && taken[index])
                        target.arguments.append(flagOf(target.arguments[k]));
                }
            }
        }
        unsigned changed = 0;
        for (auto& site : sites) {
            if (!taken[site.group] && !isCalledOnObject(site))
                continue;
            if (site.methodGetById)
                callHeadersMethodReadEarlier(site.methodGetById, site.id, site.method, flagOf(m_function.at(site.id).child(1)), yes, no);
            else
                callHeadersMethod(site.id, site.method, flagOf(m_function.at(site.id).child(0)));
            ++changed;
        }
        return changed;
    }

    bool specializeResponseConstruction(BlockID block, unsigned index)
    {
        Value original = m_function.at(m_function.at(block).values[index]);
        BuiltinName global = replaceableGlobalNameOf(original.child(0));
        if (!global || global != m_builtins.nameOf(asBytes("Response"_s.span8())) || isBehindFailedGuardOf(original.child(0), block))
            return false;
        std::optional<ResponseInit> init;
        if (original.arguments.size() == 2)
            init = responseInitLiteral(original.arguments[1], block, index);
        if (!init)
            return original.arguments.size() == 2 && specializeResponseFromResponse(block, index, global);

        removeFromBlocks(*init);
        index -= std::ranges::count_if(init->literalValues, [&](auto& moved) { return moved.block == block; });
        open(block, index);
        Value guard = make(Opcode::IsBuiltinFunction, m_program.types.boolean);
        guard.children[0] = original.child(0);
        guard.immediates[1] = global.index();
        BlockID slow = branch(append(WTF::move(guard)));
        m_calleesThatFailedGuard[slow.index()] = original.child(0);
        finish(appendThrowing(callWithResponseInit(Operations::NewResponse, original.type, original.arguments[0], *init)));

        moveTo(slow);
        appendToSlowPath(*init);
        finish(appendThrowing(WTF::move(original)));
        return true;
    }

    bool specialize(BlockID block, unsigned index)
    {
        ValueID id = m_function.at(block).values[index];
        if (m_mode == Mode::Speculation && m_function.at(id).opcode == Opcode::Construct)
            return specializeResponseConstruction(block, index);
        if (m_mode == Mode::Speculation && specializeHeaderRead(block, index))
            return true;
        Vector<Type, 4> argumentTypes;
        for (ValueID argument : m_function.at(id).arguments)
            argumentTypes.append(typeOf(argument));
        if (m_mode == Mode::Prelude) {
            BuiltinID entry = applicableEntry(firstGlobalEntryCalledBy(m_function.at(id)), argumentTypes.span());
            FunctionID callee = preludeFunctionOf(entry);
            if (!callee)
                callee = functionThatFillsSet(entry, m_function.at(id));
            if (callee) {
                Value original = m_function.at(id);
                open(block, index);
                callDirectly(callee, { }, original);
                return true;
            }
        }

        auto site = siteOf(m_function.at(id));
        if (!site)
            return false;
        BuiltinName name = m_program.builtinName(site->name);
        if (!name)
            return false;
        if (BuiltinName holder = replaceableGlobalNameOf(site->receiver))
            return m_mode == Mode::Speculation && guardCallee(block, index, *site, applicableEntry(m_builtins.find(BuiltinKind::StaticFunction, holder, name), argumentTypes.span()));
        auto& receiver = typeOf(site->receiver);
        if (BuiltinID method = m_builtins.hasOwnProperty(name); method && receiver.hasObjects() && !receiver.singleEngineClass()) {
            argumentTypes.insert(0, receiver);
            BuiltinID takingReceiver = applicableEntry(m_builtins.hasOwn(), argumentTypes.span());
            bool isCalledAsValue = !takingReceiver && argumentTypes.size() == 2 && m_builtins.hasCell(method);
            return m_mode == Mode::Speculation && guardCallee(block, index, *site, method, isCalledAsValue ? std::nullopt : std::optional(takingReceiver), isCalledAsValue);
        }
        if (m_mode == Mode::Speculation && splitOnNumber(block, index, *site, name, argumentTypes.span()))
            return true;
        const EngineClass* engineClass = receiver.singleEngineClass();
        if (!receiver.hasObjects()) {
            if (m_mode != Mode::Prelude || !(receiver.primitives & Primitive::String) || !receiver.hasOnlyPrimitives(Primitive::String | Primitive::UndefinedOrNull))
                return false;
            engineClass = &m_builtins.stringClass();
        }
        bool isKnown = engineClass;
        if (isKnown ? !(isStringClass(*engineClass) || engineClass->canCheckOriginalStructure()) : !(engineClass = classSpeculatedFor(site->receiver, name)))
            return false;
        auto& refuted = m_refuted[block.index()];
        if (refuted.engineClass == engineClass->index && refuted.receiver == originOf(site->receiver))
            return false;

        BuiltinID first = m_builtins.findForInstance(BuiltinKind::Method, *engineClass, name);
        BuiltinID entry = applicableEntry(first, argumentTypes.span());
        bool stubChecksArgument = false;
        if (!entry && !isKnown && engineClass->type == RegExpObjectType && m_mode == Mode::Speculation && (m_function.at(id).opcode == Opcode::CallMethod || m_function.at(id).opcode == Opcode::Call)) {
            Vector<Type, 4> strings;
            strings.fill(m_program.at(m_program.types.string), argumentTypes.size());
            entry = applicableEntry(first, strings.span());
            if (entry && speculatedMethod(entry, m_function.at(id).arguments) == SpeculatedMethod::None)
                return false;
            stubChecksArgument = !!entry;
        }
        if (entry && !isKnown && isStringClass(*engineClass) && m_mode == Mode::Speculation && (m_function.at(id).opcode == Opcode::CallMethod || m_function.at(id).opcode == Opcode::Call) && speculatedMethod(entry, m_function.at(id).arguments) == SpeculatedMethod::StringMatch)
            stubChecksArgument = true;
        if (!entry || m_builtins.at(entry).implementation.kind == BuiltinImplementation::Generic)
            return false;
        FunctionID callee;
        if (m_mode == Mode::Prelude) {
            callee = preludeFunctionFor(entry, m_function.at(id));
            if (!callee)
                return false;
        } else if (isKnown)
            return false;
        if ((stubChecksArgument || (!m_isStaticallyCold.isEmpty() && m_isStaticallyCold[block.index()])) && (m_function.at(id).opcode == Opcode::CallMethod || m_function.at(id).opcode == Opcode::Call)) {
            if (auto method = speculatedMethod(entry, m_function.at(id).arguments); method != SpeculatedMethod::None) {
                m_function.at(id).speculatedMethod = method;
                fuseMethodReadIntoCall(block, index);
                return false;
            }
            auto& implementation = m_builtins.at(entry).implementation;
            if ((implementation.kind == BuiltinImplementation::Call || (implementation.kind == BuiltinImplementation::Inline && implementation.inlined != InlineBuiltin::ArrayEntries)) && engineClass->index < std::numeric_limits<uint16_t>::max()) {
                fuseMethodReadIntoCall(block, index);
                if (m_function.at(id).opcode == Opcode::CallMethod) {
                    m_function.at(id).predictedReceiverClassPlusOne = engineClass->index + 1;
                    return false;
                }
            }
        }
        if (!isKnown)
            fuseMethodReadIntoCall(block, index);
        if (!callee && keepMethodRead(id, entry, *engineClass))
            return false;
        TypeID typeOfClass = m_program.engineClassTypes[engineClass->index];
        RELEASE_ASSERT(typeOfClass);

        Value original = m_function.at(id);
        FunctionID mapJoin = m_program.preludeFunctions[static_cast<unsigned>(PreludeFunction::ArrayMapJoin)];
        std::optional<unsigned> joinIndex;
        if (callee && mapJoin && callee == m_program.preludeFunctions[static_cast<unsigned>(PreludeFunction::ArrayMap)])
            joinIndex = indexOfFusedJoin(block, index);
        if (joinIndex) {
            Value join = m_function.at(m_function.at(block).values[*joinIndex]);
            Value unused = make(Opcode::Undefined, m_program.types.undefined);
            unused.position = original.position;
            unused.inlineFrame = original.inlineFrame;
            m_function.at(id) = WTF::move(unused);
            open(block, *joinIndex);
            join.arguments = { original.arguments[0], join.arguments[0] };
            callDirectly(mapJoin, site->receiver, join);
            return true;
        }
        open(block, index);
        if (callee && receiver.isString()) {
            callDirectly(callee, site->receiver, original);
            return true;
        }
        Value guard = make(Opcode::CallBuiltin, m_program.types.boolean);
        guard.immediates = { m_builtins.hasOriginalStructureOperation().index(), engineClass->index };
        guard.arguments.append(site->receiver);
        BlockID slow = branch(append(WTF::move(guard)));

        if (callee)
            callPreludeFunction(callee, site->receiver, original);
        else {
            Value call = original;
            call.opcode = Opcode::CallMethod;
            call.children = { held(site->receiver, typeOfClass), ValueID(), ValueID() };
            call.immediates = { site->name.index(), m_inliningDepth };
            finish(appendThrowing(WTF::move(call)));
        }

        moveTo(slow);
        m_refuted[slow.index()] = { originOf(site->receiver), engineClass->index };
        original.isInTailPosition &= !m_function.isGuardedPreludeEntry;
        finish(appendThrowing(WTF::move(original)));
        return true;
    }

    void fuseMethodReadIntoCall(BlockID block, unsigned index)
    {
        auto& values = m_function.at(block).values;
        auto& call = m_function.at(values[index]);
        if (call.opcode != Opcode::Call)
            return;
        ValueID read = call.child(0);
        if (m_function.at(read).opcode != Opcode::GetById || m_function.at(read).child(0) != call.child(1))
            return;
        unsigned at = index;
        while (true) {
            if (at-- == m_remainingValueCounts[block.index()])
                return;
            if (values[at] == read)
                break;
            switch (m_function.at(values[at]).opcode) {
            case Opcode::Undefined:
            case Opcode::Null:
            case Opcode::Boolean:
            case Opcode::Integer:
            case Opcode::Double:
            case Opcode::String:
            case Opcode::BigInt:
            case Opcode::FunctionReference:
            case Opcode::NewClosure:
                break;
            default:
                return;
            }
        }
        if (useCountOf(read) != 1)
            return;
        call.opcode = Opcode::CallMethod;
        call.immediates = { m_function.at(read).string().index(), call.immediates[1] };
        call.children = { call.child(1), ValueID(), ValueID() };
        Value nothing;
        nothing.opcode = Opcode::Undefined;
        nothing.position = m_function.at(read).position;
        nothing.inlineFrame = m_function.at(read).inlineFrame;
        nothing.type = m_program.types.undefined;
        nothing.rep = repOf(m_program.at(nothing.type));
        m_function.at(read) = WTF::move(nothing);
    }

    unsigned useCountOf(ValueID value) const
    {
        unsigned uses = 0;
        auto count = [&](ValueID used) {
            uses += used == value;
        };
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values)
                std::as_const(m_function.at(id)).forEachChild(count);
            block.terminator.forEachChild(count);
        }
        return uses;
    }

    bool keepMethodRead(ValueID id, BuiltinID entry, const EngineClass& engineClass)
    {
        auto& call = m_function.at(id);
        auto& implementation = m_builtins.at(entry).implementation;
        bool isLoweredWhenPredicted = implementation.kind == BuiltinImplementation::Call || (implementation.kind == BuiltinImplementation::Inline && implementation.inlined != InlineBuiltin::ArrayEntries);
        if (call.opcode != Opcode::Call || !isLoweredWhenPredicted || engineClass.index >= std::numeric_limits<uint16_t>::max() || typeOf(call.child(1)).singleLayout())
            return false;
        ValueID read = call.child(0);
        auto& method = m_function.at(read);
        if (method.opcode != Opcode::GetById || method.child(0) != call.child(1) || method.immediates[1] != Format::none || !m_program.at(method.type).isAnything() || useCountOf(read) != 1)
            return false;
        method.immediates[1] = entry.index();
        method.predictedReceiverClassPlusOne = engineClass.index + 1;
        call.opcode = Opcode::CallMethod;
        call.immediates = { method.string().index(), call.immediates[1] };
        call.children = { call.child(1), read, ValueID() };
        call.predictedReceiverClassPlusOne = engineClass.index + 1;
        return true;
    }

    SpeculatedMethod speculatedMethod(BuiltinID entry, const Arguments& arguments) const
    {
        auto& implementation = m_builtins.at(entry).implementation;
        if (implementation.kind != BuiltinImplementation::Inline || arguments.isEmpty())
            return SpeculatedMethod::None;
        size_t numberOfArguments = arguments.size();
        if (implementation.inlined == InlineBuiltin::ArrayPush)
            return numberOfArguments == 1 ? SpeculatedMethod::ArrayPush : SpeculatedMethod::None;
        auto& keyType = m_program.at(m_function.at(arguments[0]).type);
        if (!keyType.isAnything() && (!keyType.isString() || implementation.inlined == InlineBuiltin::SetAdd))
            return SpeculatedMethod::None;
        switch (implementation.inlined) {
        case InlineBuiltin::MapGet:
            return numberOfArguments == 1 ? SpeculatedMethod::MapGet : SpeculatedMethod::None;
        case InlineBuiltin::MapHas:
            return numberOfArguments == 1 ? SpeculatedMethod::MapHas : SpeculatedMethod::None;
        case InlineBuiltin::MapSet:
            return numberOfArguments == 2 ? SpeculatedMethod::MapSet : SpeculatedMethod::None;
        case InlineBuiltin::SetHas:
            return numberOfArguments == 1 ? SpeculatedMethod::SetHas : SpeculatedMethod::None;
        case InlineBuiltin::SetAdd:
            return numberOfArguments == 1 ? SpeculatedMethod::SetAdd : SpeculatedMethod::None;
        case InlineBuiltin::RegExpExec:
            return numberOfArguments == 1 ? SpeculatedMethod::RegExpExec : SpeculatedMethod::None;
        case InlineBuiltin::RegExpTest:
            return numberOfArguments == 1 ? SpeculatedMethod::RegExpTest : SpeculatedMethod::None;
        case InlineBuiltin::StringMatch:
            return numberOfArguments == 1 && keyType.isAnything() ? SpeculatedMethod::StringMatch : SpeculatedMethod::None;
        default:
            return SpeculatedMethod::None;
        }
    }

    bool splitOnNumber(BlockID block, unsigned index, Site site, BuiltinName name, std::span<const Type> argumentTypes)
    {
        auto& receiver = typeOf(site.receiver);
        TypeID prediction = m_function.at(site.receiver).prediction();
        Type withoutNullish = receiver.withoutPrimitives(Primitive::UndefinedOrNull);
        bool isNumberOrNullish = withoutNullish.isNumber();
        if (!(receiver.primitives & Primitive::Number) || receiver.isNumber() || !(isNumberOrNullish || (prediction && m_program.at(prediction).withoutPrimitives(Primitive::UndefinedOrNull).isNumber())))
            return false;
        auto& numberClass = m_builtins.numberClass();
        if (auto& refuted = m_refuted[block.index()]; refuted.engineClass == numberClass.index && refuted.receiver == originOf(site.receiver))
            return false;
        BuiltinID entry = applicableEntry(m_builtins.findForInstance(BuiltinKind::Method, numberClass, name), argumentTypes);
        if (!entry || m_builtins.at(entry).implementation.kind == BuiltinImplementation::Generic)
            return false;

        Value original = m_function.at(m_function.at(block).values[index]);
        open(block, index);
        if (isNumberOrNullish) {
            Value guard = make(Opcode::IsUndefinedOrNull, m_program.types.boolean);
            guard.children[0] = site.receiver;
            BlockID fast = branch(append(WTF::move(guard)));
            m_refuted[m_block.index()] = { originOf(site.receiver), numberClass.index };
            finish(appendThrowing(Value { original }));

            moveTo(fast);
            Value call = WTF::move(original);
            call.opcode = Opcode::CallMethod;
            call.children = { held(site.receiver, withoutNullish.isInteger() && withoutNullish.integers.isSubsetOf(Range::int32()) ? m_program.types.int32 : m_program.types.number), ValueID(), ValueID() };
            call.immediates = { site.name.index(), m_inliningDepth };
            finish(appendThrowing(WTF::move(call)));
            return true;
        }
        Value guard = make(Opcode::TypeOfIs, m_program.types.boolean);
        guard.children[0] = site.receiver;
        guard.immediates[0] = static_cast<uint32_t>(TypeofType::Number);
        BlockID slow = branch(append(WTF::move(guard)));

        Value call = original;
        call.opcode = Opcode::CallMethod;
        call.children = { held(site.receiver, m_program.types.number), ValueID(), ValueID() };
        call.immediates = { site.name.index(), m_inliningDepth };
        finish(appendThrowing(WTF::move(call)));

        moveTo(slow);
        m_refuted[slow.index()] = { originOf(site.receiver), numberClass.index };
        finish(appendThrowing(WTF::move(original)));
        return true;
    }

    BuiltinName replaceableGlobalNameOf(ValueID id) const
    {
        auto& value = m_function.at(originOf(id));
        if (value.opcode != Opcode::GetGlobal)
            return { };
        BuiltinName name = m_program.builtinName(value.string());
        return m_builtins.isReplaceable(name) ? name : BuiltinName();
    }

    bool hasGuard(ValueID method) const
    {
        for (auto& value : m_function.values) {
            if (value.opcode == Opcode::IsBuiltinFunction && value.child(0) == method)
                return true;
        }
        return false;
    }

    bool guardCallee(BlockID block, unsigned index, Site site, BuiltinID entry, std::optional<BuiltinID> takingReceiver = std::nullopt, bool isCalledAsValue = false)
    {
        if (!entry || m_builtins.at(entry).implementation.kind == BuiltinImplementation::Generic)
            return false;
        if (takingReceiver && (!*takingReceiver || m_builtins.at(*takingReceiver).implementation.kind == BuiltinImplementation::Generic))
            return false;
        Value original = m_function.at(m_function.at(block).values[index]);
        ValueID method = original.opcode == Opcode::Call ? original.child(0) : ValueID();
        if (method && hasGuard(method))
            return false;
        std::optional<ResponseInit> responseInit;
        if (auto& implementation = m_builtins.at(entry).implementation; implementation.kind == BuiltinImplementation::Inline && implementation.inlined == InlineBuiltin::ResponseJSON && original.arguments.size() == 2)
            responseInit = responseInitLiteral(original.arguments[1], block, index);
        if (responseInit) {
            removeFromBlocks(*responseInit);
            index -= std::ranges::count_if(responseInit->literalValues, [&](auto& moved) { return moved.block == block; });
        }

        open(block, index);
        if (!method) {
            Value read = make(Opcode::GetById, m_program.types.anything);
            read.children[0] = site.receiver;
            read.immediates[0] = site.name.index();
            method = appendThrowing(WTF::move(read));
        }
        Value guard = make(Opcode::IsBuiltinFunction, m_program.types.boolean);
        guard.children[0] = method;
        guard.immediates[0] = entry.index();
        BlockID slow = branch(append(WTF::move(guard)));

        Value call = make(Opcode::CallBuiltin, original.type);
        if (responseInit)
            call = callWithResponseInit(Operations::NewJSONResponse, original.type, original.arguments[0], *responseInit);
        else if (isCalledAsValue) {
            Value function = make(Opcode::BuiltinFunction, m_program.types.function);
            function.immediates = { entry.index(), Format::none };
            call.opcode = Opcode::Call;
            call.children = { append(WTF::move(function)), site.receiver, ValueID() };
            call.immediates[1] = m_inliningDepth;
            call.arguments = original.arguments;
        } else if (takingReceiver) {
            call.immediates[0] = takingReceiver->index();
            call.arguments.append(site.receiver);
            call.arguments.appendVector(original.arguments);
        } else {
            call.children[0] = site.receiver;
            call.immediates[0] = entry.index();
            call.arguments = original.arguments;
        }
        if (auto& returnedValue = m_builtins.at(isCalledAsValue ? entry : call.builtin()).result; !isCalledAsValue && returnedValue.kinds == BuiltinResult::Boolean && !returnedValue.arguments && m_program.at(call.type).isAnything()) {
            call.type = m_program.types.boolean;
            call.rep = repOf(m_program.at(call.type));
        }
        finish(appendThrowing(WTF::move(call)));

        moveTo(slow);
        if (responseInit)
            appendToSlowPath(*responseInit);
        Value general = make(Opcode::Call, original.type);
        general.children = { method, site.receiver, ValueID() };
        general.immediates[1] = m_inliningDepth;
        general.arguments = WTF::move(original.arguments);
        finish(appendThrowing(WTF::move(general)));
        return true;
    }

    void recordRefutedReceivers()
    {
        m_refuted.fill(Refuted { }, m_function.blocks.size());
        m_calleesThatFailedGuard.fill(ValueID(), m_function.blocks.size());
        m_keepsOriginalDates.fill(false, m_function.blocks.size());
        m_remainingValueCounts.fill(0, m_function.blocks.size());
        for (auto& block : m_function.blocks) {
            if (block.terminator.kind != TerminatorKind::Branch)
                continue;
            auto& condition = m_function.at(block.terminator.value);
            if (condition.comparedGlobal())
                m_calleesThatFailedGuard[block.terminator.targets[1].block.index()] = condition.child(0);
            if (condition.opcode == Opcode::CallBuiltin && condition.builtin() == m_builtins.hasOriginalStructureOperation())
                m_refuted[block.terminator.targets[1].block.index()] = { originOf(condition.arguments[0]), condition.engineClass() };
            if (condition.opcode == Opcode::TypeOfIs && condition.typeofType() == TypeofType::Number)
                m_refuted[block.terminator.targets[1].block.index()] = { originOf(condition.child(0)), m_builtins.numberClass().index };
        }
    }

    Arguments passedTo(FunctionID id, ValueID receiver, std::span<const ValueID> given)
    {
        auto& callee = m_program.at(id);
        Arguments all;
        unsigned taken = 0;
        for (unsigned k = 0; k < callee.parameterTypes.size(); ++k) {
            ValueID argument;
            if (!k && receiver)
                argument = receiver;
            else if (taken < given.size())
                argument = given[taken++];
            else
                argument = append(make(Opcode::Undefined, m_program.types.undefined));
            all.append(held(argument, callee.parameterTypes[k]));
        }
        return all;
    }

    void callDirectly(FunctionID callee, ValueID receiver, const Value& original)
    {
        Value call = make(Opcode::CallDirect, m_program.at(callee).resultType ? m_program.at(callee).resultType : m_program.types.anything);
        call.arguments = passedTo(callee, receiver, original.arguments.span());
        call.immediates = { callee.index(), m_inliningDepth };
        finish(held(appendThrowing(WTF::move(call)), original.type));
    }

    struct SortKey {
        FunctionID sort;
        StringID name;
        uint32_t flags;
    };

    std::optional<SortKey> sortKeyOfComparator(FunctionID callee, const Value& call) const
    {
        auto prelude = [&](PreludeFunction which) { return m_program.preludeFunctions[static_cast<unsigned>(which)]; };
        FunctionID sort = callee == prelude(PreludeFunction::ArraySort) ? prelude(PreludeFunction::ArraySortByKey) : callee == prelude(PreludeFunction::ArrayToSorted) ? prelude(PreludeFunction::ArrayToSortedByKey) : FunctionID();
        if (!sort || call.arguments.size() != 1)
            return std::nullopt;
        auto& passedComparator = m_function.at(originOf(call.arguments[0]));
        if (passedComparator.opcode != Opcode::FunctionReference && passedComparator.opcode != Opcode::NewClosure)
            return std::nullopt;
        auto& comparator = m_program.at(passedComparator.function());
        if (comparator.blocks.isEmpty() || comparator.has(Format::FunctionIsAsync | Format::FunctionIsGenerator) || comparator.blocks[0].parameters.size() != 2)
            return std::nullopt;
        ValueID first = comparator.blocks[0].parameters[0];
        ValueID second = comparator.blocks[0].parameters[1];
        StringID name;
        bool hasSeenKey = false;
        auto parameterOfKey = [&](ValueID id) -> std::optional<unsigned> {
            StringID read;
            if (auto& value = comparator.at(id); value.opcode == Opcode::GetById) {
                read = value.string();
                id = value.child(0);
            }
            if ((id != first && id != second) || (hasSeenKey && read != name))
                return std::nullopt;
            name = read;
            hasSeenKey = true;
            return id == second;
        };
        auto orderOfOperands = [&](const Value& value, int order) -> std::optional<int> {
            auto left = parameterOfKey(value.child(0));
            auto right = parameterOfKey(value.child(1));
            if (!left || !right || *left == *right)
                return std::nullopt;
            return *left ? -order : order;
        };

        if (auto& block = comparator.blocks[0]; block.terminator.kind == TerminatorKind::Return && block.terminator.value && comparator.at(block.terminator.value).opcode == Opcode::CallMethod) {
            static const BuiltinName localeCompare = Builtins::singleton().nameOf(asBytes("localeCompare"_s.span8()));
            auto& compared = comparator.at(block.terminator.value);
            if (comparator.blocks.size() != 1 || m_program.builtinName(compared.string()) != localeCompare || compared.arguments.size() != 1)
                return std::nullopt;
            auto left = parameterOfKey(compared.child(0));
            auto right = parameterOfKey(compared.arguments[0]);
            if (!left || !right || *left == *right || block.values.size() != (name ? 3u : 1u))
                return std::nullopt;
            return SortKey { sort, name, sortComparesByLocale | (*left ? sortIsDescending : 0) };
        }

        enum class Kind : uint8_t { Unknown, Number, Boolean };
        struct Abstract {
            Kind kind { Kind::Unknown };
            int value { 0 };
        };
        bool needsNumbers = false;
        auto signOf = [](auto number) { return number > 0 ? 1 : number < 0 ? -1 : 0; };
        auto resultFor = [&](int order) -> std::optional<int> {
            UncheckedKeyHashMap<uint32_t, Abstract, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> bound;
            auto evaluate = [&](auto& self, ValueID id) -> Abstract {
                if (auto found = bound.find(id.index()); found != bound.end())
                    return found->value;
                auto& value = comparator.at(id);
                switch (value.opcode) {
                case Opcode::Integer:
                    return { Kind::Number, signOf(value.integer()) };
                case Opcode::Double: {
                    double number = std::bit_cast<double>(value.bits());
                    return number != number ? Abstract { } : Abstract { Kind::Number, signOf(number) };
                }
                case Opcode::Boolean:
                    return { Kind::Boolean, value.boolean() };
                case Opcode::Negate: {
                    Abstract operand = self(self, value.child(0));
                    return operand.kind == Kind::Number ? Abstract { Kind::Number, -operand.value } : Abstract { };
                }
                case Opcode::LogicalNot: {
                    Abstract operand = self(self, value.child(0));
                    return operand.kind == Kind::Boolean ? Abstract { Kind::Boolean, !operand.value } : Abstract { };
                }
                case Opcode::Sub:
                case Opcode::Less:
                case Opcode::LessEq:
                case Opcode::Greater:
                case Opcode::GreaterEq:
                case Opcode::StrictEq:
                case Opcode::Eq: {
                    auto operands = orderOfOperands(value, order);
                    if (!operands)
                        return { };
                    switch (value.opcode) {
                    case Opcode::Sub:
                        needsNumbers = true;
                        return { Kind::Number, *operands };
                    case Opcode::Less:
                        return { Kind::Boolean, *operands < 0 };
                    case Opcode::LessEq:
                        return { Kind::Boolean, *operands <= 0 };
                    case Opcode::Greater:
                        return { Kind::Boolean, *operands > 0 };
                    case Opcode::GreaterEq:
                        return { Kind::Boolean, *operands >= 0 };
                    default:
                        return { Kind::Boolean, !*operands };
                    }
                }
                default:
                    return { };
                }
            };
            BlockID at(0u);
            for (unsigned steps = 0; steps < 16; ++steps) {
                auto& block = comparator.at(at);
                for (ValueID id : block.values) {
                    switch (comparator.at(id).opcode) {
                    case Opcode::GetById:
                        if (!parameterOfKey(id))
                            return std::nullopt;
                        break;
                    case Opcode::Integer:
                    case Opcode::Double:
                    case Opcode::Boolean:
                    case Opcode::Negate:
                    case Opcode::LogicalNot:
                        break;
                    default:
                        if (evaluate(evaluate, id).kind == Kind::Unknown)
                            return std::nullopt;
                    }
                }
                auto& terminator = block.terminator;
                unsigned taken = 0;
                if (terminator.kind == TerminatorKind::Return) {
                    Abstract result = terminator.value ? evaluate(evaluate, terminator.value) : Abstract { };
                    return result.kind == Kind::Number ? std::optional(result.value) : std::nullopt;
                }
                if (terminator.kind == TerminatorKind::Branch) {
                    Abstract condition = evaluate(evaluate, terminator.value);
                    if (condition.kind != Kind::Boolean)
                        return std::nullopt;
                    taken = !condition.value;
                } else if (terminator.kind != TerminatorKind::Jump)
                    return std::nullopt;
                auto& target = terminator.targets[taken];
                auto& parameters = comparator.at(target.block).parameters;
                if (target.arguments.size() != parameters.size())
                    return std::nullopt;
                Vector<Abstract, 2> passed;
                for (ValueID argument : target.arguments)
                    passed.append(evaluate(evaluate, argument));
                for (unsigned i = 0; i < passed.size(); ++i)
                    bound.set(parameters[i].index(), passed[i]);
                at = target.block;
            }
            return std::nullopt;
        };
        auto whenLess = resultFor(-1);
        auto whenEqual = resultFor(0);
        auto whenGreater = resultFor(1);
        if (!whenLess || !whenEqual || !whenGreater || *whenEqual || !*whenLess || *whenLess != -*whenGreater)
            return std::nullopt;
        return SortKey { sort, name, (*whenLess > 0 ? sortIsDescending : 0) | (needsNumbers ? 0 : sortKeysMayBeStrings) };
    }

    void callPreludeFunction(FunctionID callee, ValueID receiver, const Value& original)
    {
        auto key = sortKeyOfComparator(callee, original);
        if (!key) {
            callDirectly(callee, receiver, original);
            return;
        }
        Value byKey = original;
        Value name = make(key->name ? Opcode::String : Opcode::Undefined, key->name ? m_program.types.string : m_program.types.undefined);
        if (key->name)
            name.immediates = { key->name.index(), Format::none };
        byKey.arguments.append(append(WTF::move(name)));
        Value flags = make(Opcode::Integer, m_program.types.int32);
        flags.setBits(key->flags);
        byKey.arguments.append(append(WTF::move(flags)));
        callDirectly(key->sort, receiver, byKey);
    }

    BlockID addBlock()
    {
        m_function.blocks.append(Block { });
        m_refuted.append(Refuted { });
        m_calleesThatFailedGuard.append(ValueID());
        m_keepsOriginalDates.append(false);
        m_remainingValueCounts.append(0);
        if (!m_isStaticallyCold.isEmpty())
            m_isStaticallyCold.append(m_isStaticallyCold[m_siteBlock.index()]);
        if (!m_isInLoop.isEmpty())
            m_isInLoop.append(m_isInLoop[m_siteBlock.index()]);
        return BlockID(m_function.blocks.size() - 1);
    }

    Value make(Opcode opcode, TypeID type) const
    {
        Value value;
        value.opcode = opcode;
        value.position = m_position;
        value.inlineFrame = m_inlineFrame;
        value.isInTailPosition = m_isInTailPosition && (opcode == Opcode::Call || opcode == Opcode::CallMethod || opcode == Opcode::CallDirect || opcode == Opcode::CallBuiltin);
        value.type = type;
        value.rep = repOf(m_program.at(type));
        return value;
    }

    void open(BlockID block, unsigned index)
    {
        ValueID id = m_function.at(block).values[index];
        m_position = m_function.at(id).position;
        m_inlineFrame = m_function.at(id).inlineFrame;
        m_isInTailPosition = m_function.at(id).isInTailPosition;
        m_inliningDepth = m_function.at(id).opcode == Opcode::Construct ? Format::none : m_function.at(id).immediates[1];
        m_hasHandler = m_function.at(block).terminator.kind == TerminatorKind::Invoke && index + 1 == m_function.at(block).values.size();
        m_after = addBlock();
        {
            auto& from = m_function.at(block);
            unsigned remaining = std::exchange(m_remainingValueCounts[block.index()], 0);
            BlockValues ahead;
            ahead.append(from.values.subspan(remaining, index - remaining));
            m_function.at(m_after).values = std::exchange(from.values, WTF::move(ahead));
            m_remainingValueCounts[m_after.index()] = index + 1;
            m_function.at(m_after).terminator = std::exchange(from.terminator, Terminator { });
        }
        m_handler = { };
        if (m_hasHandler) {
            auto& invoke = m_function.at(m_after).terminator;
            m_handler = invoke.targets[1];
            invoke.kind = TerminatorKind::Jump;
            invoke.targets.shrink(1);
        }
        Value parameter = make(Opcode::Parameter, m_function.at(id).type);
        parameter.copyPredictionFrom(m_function.at(id));
        parameter.immediates = { m_after.index(), 0 };
        m_function.at(id) = WTF::move(parameter);
        m_function.at(m_after).parameters.append(id);
        moveTo(block);
    }

    void moveTo(BlockID block)
    {
        m_block = block;
        m_lastMayThrow = false;
    }

    void endBlockAfterThrowingValue()
    {
        if (!m_lastMayThrow)
            return;
        m_lastMayThrow = false;
        BlockID next = addBlock();
        auto& invoke = m_function.at(m_block).terminator;
        invoke.kind = TerminatorKind::Invoke;
        invoke.targets.append(Target { next, { } });
        invoke.targets.append(m_handler);
        m_block = next;
    }

    ValueID append(Value&& value)
    {
        endBlockAfterThrowingValue();
        m_function.values.append(WTF::move(value));
        ValueID id(m_function.values.size() - 1);
        m_function.at(m_block).values.append(id);
        return id;
    }

    ValueID appendThrowing(Value&& value)
    {
        ValueID id = append(WTF::move(value));
        m_lastMayThrow = m_hasHandler;
        return id;
    }

    BlockID branch(ValueID condition)
    {
        endBlockAfterThrowingValue();
        BlockID then = addBlock();
        BlockID otherwise = addBlock();
        auto& branch = m_function.at(m_block).terminator;
        branch.kind = TerminatorKind::Branch;
        branch.value = condition;
        branch.targets.append(Target { then, { } });
        branch.targets.append(Target { otherwise, { } });
        moveTo(then);
        return otherwise;
    }

    void finish(ValueID result)
    {
        auto& terminator = m_function.at(m_block).terminator;
        terminator.kind = m_lastMayThrow ? TerminatorKind::Invoke : TerminatorKind::Jump;
        terminator.targets.append(Target { m_after, Arguments { result } });
        if (m_lastMayThrow)
            terminator.targets.append(m_handler);
        m_lastMayThrow = false;
    }

    ValueID held(ValueID value, TypeID type)
    {
        if (!type || m_program.castIsRedundant(typeOf(value), m_program.at(type), CastReason::Claim))
            return value;
        Value cast = make(Opcode::Cast, type);
        cast.children[0] = value;
        cast.immediates[1] = static_cast<uint32_t>(CastReason::Claim);
        return appendThrowing(WTF::move(cast));
    }

    const Program& m_program;
    Function& m_function;
    const Builtins& m_builtins;
    Mode m_mode;
    Vector<Refuted> m_refuted;
    Vector<ValueID> m_calleesThatFailedGuard;
    Vector<bool> m_keepsOriginalDates;
    Vector<unsigned> m_remainingValueCounts;
    Vector<bool> m_isStaticallyCold;
    Vector<bool> m_isInLoop;
    BlockID m_siteBlock;
    uint32_t m_position { 0 };
    uint32_t m_inlineFrame { 0 };
    uint32_t m_inliningDepth { Format::none };
    bool m_isInTailPosition { false };
    bool m_hasHandler { false };
    Target m_handler;
    BlockID m_after;
    BlockID m_block;
    bool m_lastMayThrow { false };
};

class ReceiverGuardElimination {
public:
    ReceiverGuardElimination(const Program& program, Function& function)
        : m_program(program)
        , m_function(function)
        , m_builtins(Builtins::singleton())
    {
    }

    unsigned run()
    {
        collectReceivers();
        if (m_receivers.isEmpty())
            return 0;
        findUses();
        computePredecessors();
        unsigned eliminated = 0;
        for (auto& receiver : m_receivers)
            eliminated += eliminate(receiver);
        return eliminated;
    }

    uint32_t freshResultClass()
    {
        uint32_t engineClass = Format::none;
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            auto& block = m_function.blocks[b];
            if (block.terminator.kind != TerminatorKind::Return)
                continue;
            if (!block.terminator.value)
                return Format::none;
            ValueID allocation = rootOf(block.terminator.value);
            uint32_t allocatedClass = allocatedClassOf(m_function.at(allocation));
            if (allocatedClass == Format::none || (engineClass != Format::none && allocatedClass != engineClass) || !m_builtins.classes()[allocatedClass].canCheckOriginalStructure())
                return Format::none;
            engineClass = allocatedClass;
            ensureReceiver(allocation, allocatedClass).returns.append({ b, static_cast<unsigned>(block.values.size()) });
        }
        if (engineClass == Format::none)
            return Format::none;
        collectReceivers();
        findUses();
        computePredecessors();
        for (auto& receiver : m_receivers) {
            if (receiver.returns.isEmpty())
                continue;
            if (receiver.definition.block == none)
                return Format::none;
            propagateEscapes(receiver);
            for (Location location : receiver.returns) {
                if (isAfterEscape(location))
                    return Format::none;
            }
        }
        return engineClass;
    }

    ValueID allocationOf(ValueID id) { return rootOf(id); }
    uint32_t allocationClass(ValueID id) { return allocatedClassOf(m_function.at(rootOf(id))); }

    void removeUnreachableBlocks()
    {
        BitVector isReached;
        Vector<unsigned, 16> worklist { 0u };
        while (!worklist.isEmpty()) {
            unsigned block = worklist.takeLast();
            if (isReached.set(block))
                continue;
            for (auto& target : m_function.blocks[block].terminator.targets)
                worklist.append(target.block.index());
        }
        Value undefinedValue;
        undefinedValue.opcode = Opcode::Undefined;
        undefinedValue.type = m_program.types.undefined;
        undefinedValue.rep = repOf(m_program.at(undefinedValue.type));
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            if (isReached.get(b))
                continue;
            for (ValueID id : m_function.blocks[b].values)
                m_function.at(id) = undefinedValue;
            for (ValueID id : m_function.blocks[b].parameters)
                m_function.at(id) = undefinedValue;
            m_function.blocks[b] = Block { };
        }
        compactBlocks(m_function);
    }

private:
    static constexpr unsigned none = UINT_MAX;

    struct Location {
        unsigned block { none };
        unsigned index { none };
    };

    struct Guard {
        ValueID id;
        Location location;
        bool holds { true };
        BitVector failureRegion;
    };

    struct Call {
        ValueID id;
        Location location;
    };

    struct Receiver {
        ValueID allocation;
        uint32_t engineClass { Format::none };
        Location definition;
        Vector<Guard, 2> guards;
        Vector<Call, 4> calls;
        Vector<Location, 1> returns;
        Vector<ValueID, 2> casts;
        Vector<Location, 4> escapes;
    };

    bool isGuard(const Value& value) const
    {
        return value.opcode == Opcode::CallBuiltin && value.builtin() == m_builtins.hasOriginalStructureOperation();
    }

    ValueID rootOf(ValueID id)
    {
        if (m_roots.isEmpty())
            m_roots.fill(ValueID(), m_function.values.size());
        ValueID& known = m_roots[id.index()];
        if (known)
            return known;
        ValueID root = id;
        for (;;) {
            root = m_program.originOf(m_function, root);
            auto& value = m_function.at(root);
            if (value.opcode != Opcode::CallBuiltin)
                break;
            auto& result = m_builtins.at(value.builtin()).result;
            ValueID operand;
            if (result.kinds == BuiltinResult::This && !result.arguments)
                operand = value.child(0);
            else if (!result.kinds && hasOneBitSet(result.arguments) && getLSBSet(result.arguments) < value.arguments.size())
                operand = value.arguments[getLSBSet(result.arguments)];
            if (!operand)
                break;
            root = operand;
        }
        m_roots[id.index()] = root;
        return root;
    }

    static bool returnsFreshObject(const Builtin& builtin)
    {
        if (builtin.kind == BuiltinKind::Constructor || builtin.implementation.kind == BuiltinImplementation::Prelude)
            return true;
        if (builtin.implementation.kind != BuiltinImplementation::Inline)
            return false;
        switch (builtin.implementation.inlined) {
        case InlineBuiltin::ArrayOf:
        case InlineBuiltin::ArrayWithLength:
        case InlineBuiltin::ArrayFrom:
        case InlineBuiltin::NewArrayWithCapacity:
        case InlineBuiltin::NewArrayWithSpecies:
        case InlineBuiltin::NewContiguousArray:
        case InlineBuiltin::NewDoubleArray:
        case InlineBuiltin::TruncateNewArray:
        case InlineBuiltin::StringSplit:
        case InlineBuiltin::ObjectEntries:
        case InlineBuiltin::ObjectGetOwnPropertyNames:
        case InlineBuiltin::ObjectGetOwnPropertySymbols:
        case InlineBuiltin::ObjectKeys:
        case InlineBuiltin::ObjectValues:
        case InlineBuiltin::ReflectOwnKeys:
        case InlineBuiltin::RegExpNew:
            return true;
        default:
            return false;
        }
    }

    uint32_t allocatedClassOf(const Value& value) const
    {
        auto classOf = [&](BuiltinID entry) -> uint32_t {
            auto& builtin = m_builtins.at(entry);
            if (builtin.result.kinds != BuiltinResult::Instance || builtin.result.arguments || !returnsFreshObject(builtin))
                return Format::none;
            return builtin.result.engineClass;
        };
        switch (value.opcode) {
        case Opcode::NewArray:
        case Opcode::CreateRest:
            return m_builtins.arrayClass().index;
        case Opcode::CallBuiltin:
            return m_builtins.at(value.builtin()).implementation.kind == BuiltinImplementation::Prelude ? Format::none : classOf(value.builtin());
        case Opcode::CallDirect:
            for (unsigned i = 0; i < m_program.preludeFunctions.size(); ++i) {
                if (m_program.preludeFunctions[i] != value.function())
                    continue;
                BuiltinID entry = m_builtins.entryImplementedBy(static_cast<PreludeFunction>(i));
                return entry ? classOf(entry) : Format::none;
            }
            if (value.function().index() < m_program.freshResultClasses.size() && !value.constructs && m_program.freshResultClasses[value.function().index()] != UINT16_MAX)
                return m_program.freshResultClasses[value.function().index()];
            return Format::none;
        default:
            return Format::none;
        }
    }

    void collectReceivers()
    {
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            auto& values = m_function.blocks[b].values;
            for (unsigned i = 0; i < values.size(); ++i) {
                auto& value = m_function.at(values[i]);
                if (value.opcode != Opcode::CallBuiltin || value.engineClass() == Format::none || !m_builtins.classes()[value.engineClass()].canCheckOriginalStructure())
                    continue;
                bool isGuardValue = isGuard(value);
                if (!isGuardValue && !hasGenericFallback(m_program, m_function, value))
                    continue;
                ValueID allocation = rootOf(isGuardValue ? value.arguments[0] : value.child(0));
                if (allocatedClassOf(m_function.at(allocation)) != value.engineClass())
                    continue;
                if (isGuardValue)
                    ensureReceiver(allocation, value.engineClass()).guards.append(Guard { values[i], { b, i }, true, { } });
                else
                    ensureReceiver(allocation, value.engineClass()).calls.append(Call { values[i], { b, i } });
            }
        }
    }

    Receiver& ensureReceiver(ValueID allocation, uint32_t engineClass)
    {
        if (m_receiverIndex.isEmpty())
            m_receiverIndex.fill(none, m_function.values.size());
        unsigned& number = m_receiverIndex[allocation.index()];
        if (number == none) {
            number = m_receivers.size();
            m_receivers.append(Receiver { allocation, engineClass, { }, { }, { }, { }, { }, { } });
        }
        return m_receivers[number];
    }

    bool doesNotEscapeThrough(ValueID id, const Value& user, const Receiver& receiver)
    {
        auto aliasesReceiver = [&](ValueID operand) {
            return operand && rootOf(operand) == receiver.allocation;
        };
        switch (user.opcode) {
        case Opcode::Cast:
            return rootOf(id) == receiver.allocation;
        case Opcode::GetById:
        case Opcode::InById:
        case Opcode::StrictEq:
        case Opcode::LogicalNot:
        case Opcode::ToBoolean:
        case Opcode::TypeOf:
        case Opcode::TypeOfIs:
        case Opcode::IsUndefinedOrNull:
        case Opcode::IsInstanceOfClass:
        case Opcode::IteratorOpen:
            return true;
        case Opcode::CallMethod: {
            for (ValueID argument : user.arguments) {
                if (aliasesReceiver(argument))
                    return false;
            }
            BuiltinName name = m_program.builtinName(user.string());
            BuiltinID entry = name ? m_builtins.findForInstance(BuiltinKind::Method, m_builtins.classes()[receiver.engineClass], name) : BuiltinID();
            if (!entry)
                return false;
            uint16_t uses = 0;
            for (; entry; entry = m_builtins.next(entry))
                uses |= m_builtins.at(entry).thisUses;
            return !(uses & (OperandUse::WritesProperties | OperandUse::Keeps | OperandUse::Calls | OperandUse::MayBeResult | OperandUse::EscapesToUserCode));
        }
        case Opcode::GetByVal:
            return !aliasesReceiver(user.child(1));
        case Opcode::PutByVal: {
            auto& key = m_program.typeOf(m_function, user.child(1));
            return receiver.engineClass == m_builtins.arrayClass().index && !aliasesReceiver(user.child(1)) && !aliasesReceiver(user.child(2)) && key.isInteger() && key.integers.isSubsetOf(Range::length());
        }
        case Opcode::CallBuiltin: {
            if (isGuard(user))
                return true;
            auto& builtin = m_builtins.at(user.builtin());
            if (builtin.implementation.kind == BuiltinImplementation::Inline && builtin.implementation.inlined == InlineBuiltin::AwaitBySavingFrame)
                return !std::ranges::any_of(user.arguments.subspan(0, 2), aliasesReceiver);
            if (aliasesReceiver(user.child(1)) || aliasesReceiver(user.child(2)) || (aliasesReceiver(user.child(0)) && !builtin.hasReceiver()))
                return false;
            uint16_t uses = aliasesReceiver(user.child(0)) ? builtin.thisUses : 0;
            bool isPassedAsArgument = false;
            for (unsigned i = 0; i < user.arguments.size(); ++i) {
                if (!aliasesReceiver(user.arguments[i]))
                    continue;
                isPassedAsArgument = true;
                uses |= builtin.argumentUses(i);
            }
            if (uses & (OperandUse::WritesProperties | OperandUse::Keeps | OperandUse::Calls | OperandUse::EscapesToUserCode))
                return false;
            if (uses & OperandUse::MayBeResult && rootOf(id) != receiver.allocation)
                return false;
            if (!(uses & OperandUse::WritesContents))
                return true;
            if (builtin.kind == BuiltinKind::Operation)
                return true;
            return builtin.hasReceiver() && !isPassedAsArgument && user.engineClass() == receiver.engineClass;
        }
        default:
            return false;
        }
    }

    void findUses()
    {
        Vector<unsigned, 4> used;
        auto note = [&](ValueID operand) {
            unsigned number = m_receiverIndex[rootOf(operand).index()];
            if (number != none && !used.contains(number))
                used.append(number);
        };
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            auto& block = m_function.blocks[b];
            for (unsigned i = 0; i < block.values.size(); ++i) {
                ValueID id = block.values[i];
                if (unsigned number = m_receiverIndex[id.index()]; number != none)
                    m_receivers[number].definition = { b, i };
                used.shrink(0);
                std::as_const(m_function.at(id)).forEachChild(note);
                for (unsigned number : used) {
                    if (!doesNotEscapeThrough(id, m_function.at(id), m_receivers[number]))
                        m_receivers[number].escapes.append({ b, i });
                    else if (m_function.at(id).opcode == Opcode::Cast)
                        m_receivers[number].casts.append(id);
                }
            }
            used.shrink(0);
            block.terminator.forEachChild(note);
            for (unsigned number : used)
                m_receivers[number].escapes.append({ b, static_cast<unsigned>(block.values.size()) });
        }
    }

    void computePredecessors()
    {
        m_predecessors.resize(m_function.blocks.size());
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            for (auto& target : m_function.blocks[b].terminator.targets)
                m_predecessors[target.block.index()].append(b);
        }
    }

    void computeFailureRegion(Guard& guard)
    {
        auto& terminator = m_function.blocks[guard.location.block].terminator;
        if (terminator.kind != TerminatorKind::Branch || terminator.value != guard.id || terminator.targets[0].block == terminator.targets[1].block)
            return;
        unsigned first = terminator.targets[1].block.index();
        if (m_predecessors[first].size() != 1)
            return;
        auto& region = guard.failureRegion;
        region.set(first);
        Vector<unsigned, 8> worklist { first };
        while (!worklist.isEmpty()) {
            for (auto& target : m_function.blocks[worklist.takeLast()].terminator.targets) {
                unsigned next = target.block.index();
                if (region.get(next))
                    continue;
                bool isSubsetOf = true;
                for (unsigned predecessor : m_predecessors[next])
                    isSubsetOf &= region.get(predecessor);
                if (!isSubsetOf)
                    continue;
                region.set(next);
                worklist.append(next);
            }
        }
    }

    bool isAfterEscape(Location location) const
    {
        if (m_isReachableAfterEscape.get(location.block))
            return true;
        for (Location escape : m_escapes) {
            if (escape.block == location.block && escape.index < location.index)
                return true;
        }
        return false;
    }

    void propagateEscapes(Receiver& receiver)
    {
        for (auto& guard : receiver.guards)
            computeFailureRegion(guard);
        for (bool changed = true; changed;) {
            changed = false;
            m_isReachableAfterEscape.clearAll();
            m_escapes.shrink(0);
            Vector<unsigned, 8> worklist;
            auto pushSuccessors = [&](unsigned block) {
                for (auto& target : m_function.blocks[block].terminator.targets)
                    worklist.append(target.block.index());
            };
            for (Location escape : receiver.escapes) {
                bool isInFailureRegion = false;
                for (auto& guard : receiver.guards)
                    isInFailureRegion |= guard.holds && guard.failureRegion.get(escape.block);
                if (isInFailureRegion)
                    continue;
                m_escapes.append(escape);
                pushSuccessors(escape.block);
            }
            while (!worklist.isEmpty()) {
                unsigned block = worklist.takeLast();
                if (block == receiver.definition.block || m_isReachableAfterEscape.set(block))
                    continue;
                pushSuccessors(block);
            }
            for (auto& guard : receiver.guards) {
                if (!guard.holds || !isAfterEscape(guard.location))
                    continue;
                guard.holds = false;
                changed = true;
            }
        }
    }

    unsigned eliminate(Receiver& receiver)
    {
        if (receiver.definition.block == none)
            return 0;
        propagateEscapes(receiver);
        unsigned eliminated = 0;
        for (auto& guard : receiver.guards) {
            if (!guard.holds)
                continue;
            auto& value = m_function.at(guard.id);
            if (Options::validateAOTTypedFacts()) [[unlikely]] {
                value.receiverHasOriginalStructure = true;
                continue;
            }
            value.opcode = Opcode::Boolean;
            value.children = { };
            value.arguments.clear();
            value.immediates = { 1, Format::none };
            if (auto& terminator = m_function.blocks[guard.location.block].terminator; terminator.kind == TerminatorKind::Branch && terminator.value == guard.id) {
                terminator.kind = TerminatorKind::Jump;
                terminator.value = ValueID();
                terminator.targets.shrink(1);
            }
            ++eliminated;
        }
        for (auto& call : receiver.calls) {
            if (isAfterEscape(call.location))
                continue;
            auto& value = m_function.at(call.id);
            value.receiverHasOriginalStructure = true;
            ValueID read = std::exchange(value.children[1], ValueID());
            if (!read || m_function.at(read).opcode != Opcode::GetById || m_function.at(read).methodGetByIdBuiltin() != value.builtin())
                continue;
            Value undefinedValue;
            undefinedValue.opcode = Opcode::Undefined;
            undefinedValue.position = m_function.at(read).position;
            undefinedValue.inlineFrame = m_function.at(read).inlineFrame;
            undefinedValue.type = m_program.types.undefined;
            undefinedValue.rep = repOf(m_program.at(undefinedValue.type));
            m_function.at(read) = WTF::move(undefinedValue);
            ++eliminated;
        }
        for (ValueID cast : receiver.casts) {
            auto* engineClass = m_program.at(m_function.at(cast).type).singleEngineClass();
            m_function.at(cast).operandClassIsProven = engineClass && engineClass->index == receiver.engineClass;
        }
        return eliminated;
    }

    const Program& m_program;
    Function& m_function;
    const Builtins& m_builtins;
    Vector<ValueID> m_roots;
    Vector<unsigned> m_receiverIndex;
    Vector<Receiver, 2> m_receivers;
    Vector<Vector<unsigned, 2>> m_predecessors;
    BitVector m_isReachableAfterEscape;
    Vector<Location, 4> m_escapes;
};

struct ModuleVariableFacts {
    static constexpr uint32_t none = UINT32_MAX;
    static constexpr uint16_t isNeverStored = UINT16_MAX - 1;
    static constexpr uint16_t escapes = UINT16_MAX;

    bool isCandidate(uint32_t variable) const { return classes[variable] < isNeverStored; }

    Vector<uint16_t> classes;
    BitVector mayHoldNullish;
    Vector<uint32_t> returnedVariables;
    Vector<BuiltinID> loweredMethods;
};

class ModuleVariableAliases {
public:
    static constexpr uint32_t none = ModuleVariableFacts::none;

    ModuleVariableAliases(const Program& program, FunctionID id, const ModuleVariableFacts& facts)
        : m_program(program)
        , m_id(id)
        , m_function(program.at(id))
        , m_facts(facts)
        , m_builtins(Builtins::singleton())
    {
    }

    bool find()
    {
        static constexpr OpcodeSet sources { Opcode::GetModuleVariable, Opcode::PutModuleVariable, Opcode::CallDirect };
        m_program.functions.forEachIndexedValue(m_id.index(), sources, [&](ValueID id) {
            auto& value = m_function.at(id);
            uint32_t variable = value.opcode == Opcode::CallDirect ? m_facts.returnedVariables[value.function().index()] : value.variable().index();
            if (variable == none || !m_facts.isCandidate(variable))
                return;
            ValueID alias = value.opcode == Opcode::PutModuleVariable ? allocationStoredBy(value) : id;
            if (!typeOf(alias).hasObjects())
                return;
            if (m_variableOf.isEmpty())
                m_variableOf.fill(none, m_function.values.size());
            add(alias, variable);
            if (value.opcode == Opcode::GetModuleVariable && (m_program.variables[variable].startsUndefined || m_program.variables[variable].startsZero || m_facts.mayHoldNullish.get(variable)))
                m_mayBeNullish.set(id.index());
        });
        if (m_variableOf.isEmpty())
            return false;
        for (bool changed = true; changed;) {
            changed = false;
            for (auto& block : m_function.blocks) {
                for (ValueID id : block.values) {
                    if (ValueID source = sameObjectAs(id); source && variableOf(source) != none)
                        changed |= add(id, variableOf(source));
                }
                for (auto& target : block.terminator.targets) {
                    auto& parameters = m_function.at(target.block).parameters;
                    for (unsigned i = 0; i < target.arguments.size(); ++i) {
                        if (variableOf(target.arguments[i]) != none)
                            changed |= add(parameters[i], variableOf(target.arguments[i]));
                    }
                }
            }
        }
        for (bool changed = true; changed;) {
            changed = false;
            for (auto& block : m_function.blocks) {
                for (ValueID id : block.values) {
                    ValueID source = sameObjectAs(id);
                    if (!source || variableOf(source) == none)
                        continue;
                    if (m_mayBeAnotherObject.get(source.index()))
                        changed |= !m_mayBeAnotherObject.set(id.index());
                    if (m_mayBeOtherPrimitive.get(source.index()))
                        changed |= !m_mayBeOtherPrimitive.set(id.index());
                    if (m_mayBeNullish.get(source.index()) && m_function.at(id).opcode == Opcode::Cast && typeOf(id).mayBeUndefinedOrNull())
                        changed |= !m_mayBeNullish.set(id.index());
                }
                for (auto& target : block.terminator.targets) {
                    auto& parameters = m_function.at(target.block).parameters;
                    for (unsigned i = 0; i < target.arguments.size(); ++i) {
                        if (variableOf(parameters[i]) == none)
                            continue;
                        ValueID argument = target.arguments[i];
                        bool isAlias = variableOf(argument) != none;
                        auto& type = typeOf(argument);
                        if (isAlias ? m_mayBeAnotherObject.get(argument.index()) : type.hasObjects())
                            changed |= !m_mayBeAnotherObject.set(parameters[i].index());
                        if (isAlias ? m_mayBeOtherPrimitive.get(argument.index()) : !!(type.primitives & ~Primitive::UndefinedOrNull))
                            changed |= !m_mayBeOtherPrimitive.set(parameters[i].index());
                        if (isAlias ? m_mayBeNullish.get(argument.index()) : type.mayBeUndefinedOrNull())
                            changed |= !m_mayBeNullish.set(parameters[i].index());
                    }
                }
            }
        }
        return true;
    }

    void forEachEscape(const Invocable<void(uint32_t)> auto& functor)
    {
        for (uint32_t variable : m_singleValueVariables)
            functor(variable);
        computeFailureRegions();
        Vector<uint32_t, 4> used;
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            auto& block = m_function.blocks[b];
            auto isReached = [&](uint32_t variable) {
                return !m_failureRegions.containsIf([&](auto& region) {
                    return region.variable == variable && region.blocks.get(b);
                });
            };
            for (ValueID id : block.values) {
                used.shrink(0);
                m_function.at(id).forEachChild([&](ValueID operand) {
                    if (uint32_t variable = variableOf(operand); variable != none && !used.contains(variable))
                        used.append(variable);
                });
                for (uint32_t variable : used) {
                    if (isReached(variable) && !isHarmless(id, variable))
                        functor(variable);
                }
            }
            auto& terminator = block.terminator;
            uint32_t variable = terminator.value ? variableOf(terminator.value) : none;
            if (variable == none || terminator.kind == TerminatorKind::Branch || !isReached(variable))
                continue;
            if (terminator.kind != TerminatorKind::Return || m_facts.returnedVariables[m_id.index()] != variable)
                functor(variable);
        }
    }

    std::pair<unsigned, unsigned> eliminate(Function& function)
    {
        ASSERT(&function == &m_function);
        unsigned eliminated = 0;
        unsigned numberOfCalls = 0;
        auto isProven = [&](ValueID operand, uint32_t engineClass) {
            if (!operand || variableOf(operand) == none || m_mayBeAnotherObject.get(operand.index()) || m_facts.classes[variableOf(operand)] != engineClass)
                return false;
            return !typeOf(operand).primitives || (!m_mayBeNullish.get(operand.index()) && !m_mayBeOtherPrimitive.get(operand.index()));
        };
        for (auto& block : function.blocks) {
            for (ValueID id : block.values) {
                auto& value = function.at(id);
                if (value.opcode == Opcode::Cast) {
                    auto* engineClass = m_program.at(value.type).singleEngineClass();
                    if (engineClass && isProven(value.child(0), engineClass->index))
                        value.operandClassIsProven = true;
                    continue;
                }
                if (value.opcode != Opcode::CallBuiltin || value.engineClass() == Format::none)
                    continue;
                if (value.builtin() == m_builtins.hasOriginalStructureOperation()) {
                    if (!isProven(value.arguments[0], value.engineClass()))
                        continue;
                    if (Options::validateAOTTypedFacts()) [[unlikely]] {
                        value.receiverHasOriginalStructure = true;
                        continue;
                    }
                    value.opcode = Opcode::Boolean;
                    value.children = { };
                    value.arguments.clear();
                    value.immediates = { 1, Format::none };
                    if (auto& terminator = block.terminator; terminator.kind == TerminatorKind::Branch && terminator.value == id) {
                        terminator.kind = TerminatorKind::Jump;
                        terminator.value = ValueID();
                        terminator.targets.shrink(1);
                    }
                    ++eliminated;
                    continue;
                }
                if (value.receiverHasOriginalStructure || !isProven(value.child(0), value.engineClass()) || !hasGenericFallback(m_program, function, value))
                    continue;
                value.receiverHasOriginalStructure = true;
                ++numberOfCalls;
                ValueID read = std::exchange(value.children[1], ValueID());
                if (!read || function.at(read).opcode != Opcode::GetById || function.at(read).methodGetByIdBuiltin() != value.builtin())
                    continue;
                Value undefinedValue;
                undefinedValue.opcode = Opcode::Undefined;
                undefinedValue.position = function.at(read).position;
                undefinedValue.inlineFrame = function.at(read).inlineFrame;
                undefinedValue.type = m_program.types.undefined;
                undefinedValue.rep = repOf(m_program.at(undefinedValue.type));
                function.at(read) = WTF::move(undefinedValue);
                ++eliminated;
            }
        }
        return { eliminated, numberOfCalls };
    }

private:
    struct FailureRegion {
        uint32_t variable;
        BitVector blocks;
    };

    const Type& typeOf(ValueID id) const { return m_program.typeOf(m_function, id); }
    uint32_t variableOf(ValueID id) const { return m_variableOf[id.index()]; }

    bool add(ValueID id, uint32_t variable)
    {
        uint32_t& known = m_variableOf[id.index()];
        if (known == variable)
            return false;
        if (known == none) {
            known = variable;
            return true;
        }
        if (!m_singleValueVariables.contains(known))
            m_singleValueVariables.append(known);
        if (!m_singleValueVariables.contains(variable))
            m_singleValueVariables.append(variable);
        return false;
    }

    ValueID sameObjectAs(ValueID id) const
    {
        auto& value = m_function.at(id);
        if (value.opcode == Opcode::Cast)
            return m_program.originOf(m_function, id) == id ? ValueID() : value.child(0);
        if (value.opcode != Opcode::CallBuiltin)
            return { };
        auto& result = m_builtins.at(value.builtin()).result;
        if (result.kinds == BuiltinResult::This && !result.arguments)
            return value.child(0);
        if (!result.kinds && hasOneBitSet(result.arguments) && getLSBSet(result.arguments) < value.arguments.size())
            return value.arguments[getLSBSet(result.arguments)];
        return { };
    }

    ValueID allocationStoredBy(const Value& store) const
    {
        ValueID id = store.child(0);
        while (ValueID source = sameObjectAs(id))
            id = source;
        return id;
    }

    void computeFailureRegions()
    {
        Vector<Vector<unsigned, 2>> predecessors;
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            auto& terminator = m_function.blocks[b].terminator;
            if (terminator.kind != TerminatorKind::Branch || terminator.targets[0].block == terminator.targets[1].block)
                continue;
            auto& guard = m_function.at(terminator.value);
            if (guard.opcode != Opcode::CallBuiltin || guard.builtin() != m_builtins.hasOriginalStructureOperation())
                continue;
            uint32_t variable = variableOf(guard.arguments[0]);
            if (variable == none || m_facts.classes[variable] != guard.engineClass())
                continue;
            if (predecessors.isEmpty()) {
                predecessors.resize(m_function.blocks.size());
                for (unsigned from = 0; from < m_function.blocks.size(); ++from) {
                    for (auto& target : m_function.blocks[from].terminator.targets)
                        predecessors[target.block.index()].append(from);
                }
            }
            unsigned first = terminator.targets[1].block.index();
            if (predecessors[first].size() != 1)
                continue;
            BitVector region;
            region.set(first);
            Vector<unsigned, 8> worklist { first };
            while (!worklist.isEmpty()) {
                for (auto& target : m_function.blocks[worklist.takeLast()].terminator.targets) {
                    unsigned next = target.block.index();
                    if (region.get(next))
                        continue;
                    bool isSubsetOf = true;
                    for (unsigned predecessor : predecessors[next])
                        isSubsetOf &= region.get(predecessor);
                    if (!isSubsetOf)
                        continue;
                    region.set(next);
                    worklist.append(next);
                }
            }
            m_failureRegions.append({ variable, WTF::move(region) });
        }
    }

    bool isRetainedOnlyByReturnedIterator(const Builtin& builtin) const
    {
        return builtin.kind == BuiltinKind::Method && builtin.thisUses == OperandUse::Keeps && (builtin.name == "entries"_s || builtin.name == "keys"_s || builtin.name == "values"_s || builtin.name == "@@iterator"_s);
    }

    bool readsUnnamedArguments(FunctionID id) const
    {
        return id && m_program.at(id).values.containsIf([&](auto& value) {
            switch (value.opcode) {
            case Opcode::GetArgument:
            case Opcode::CreateRest:
            case Opcode::GetRestLength:
            case Opcode::GetMyArgumentByVal:
            case Opcode::CallForwardVarargs:
            case Opcode::ConstructSuperForwardVarargs:
                return true;
            case Opcode::Operation:
            case Opcode::CallBuiltin:
                return languageOperationOf(m_program, value) == LanguageOperation::CreateArguments || languageOperationOf(m_program, value) == LanguageOperation::DirectEval;
            default:
                return false;
            }
        });
    }

    bool takesAtMostTwoArguments(ValueID id) const
    {
        auto& callback = m_function.at(m_program.originOf(m_function, id));
        if (callback.opcode != Opcode::NewClosure && callback.opcode != Opcode::FunctionReference)
            return false;
        auto& callee = m_program.at(callback.function());
        if (callee.blocks.isEmpty() || callee.parameterTypes.size() > 2 || callee.declaredAs || callee.resumeFunction || callee.startFunction || callee.has(Format::FunctionHasRestParameter | Format::FunctionIsSloppy))
            return false;
        FunctionID generalEntry = m_program.genericEntrypoints.get(callback.function().index());
        for (FunctionID body : { callback.function(), generalEntry, generalEntry ? m_program.at(generalEntry).valueEntry : FunctionID(), callee.rampForClosures, callee.declaredGenericEntrypoint, callee.untypedClone, callee.worker }) {
            if (readsUnnamedArguments(body))
                return false;
        }
        return true;
    }

    bool isHarmless(ValueID id, uint32_t variable) const
    {
        auto& user = m_function.at(id);
        uint32_t engineClass = m_facts.classes[variable];
        auto aliases = [&](ValueID operand) {
            return operand && variableOf(operand) == variable;
        };
        constexpr uint16_t harmfulUses = OperandUse::WritesProperties | OperandUse::Keeps | OperandUse::Calls | OperandUse::EscapesToUserCode;
        switch (user.opcode) {
        case Opcode::Cast:
            return !!sameObjectAs(id);
        case Opcode::GetById:
        case Opcode::InById:
        case Opcode::StrictEq:
        case Opcode::LogicalNot:
        case Opcode::ToBoolean:
        case Opcode::TypeOf:
        case Opcode::TypeOfIs:
        case Opcode::IsUndefinedOrNull:
        case Opcode::IsInstanceOfClass:
        case Opcode::IteratorOpen:
            return true;
        case Opcode::PutModuleVariable:
            return user.variable().index() == variable;
        case Opcode::CallMethod: {
            if (std::ranges::any_of(user.arguments, aliases))
                return false;
            BuiltinName name = m_program.builtinName(user.string());
            BuiltinID entry = name ? m_builtins.findForInstance(BuiltinKind::Method, m_builtins.classes()[engineClass], name) : BuiltinID();
            if (!entry)
                return false;
            uint16_t uses = 0;
            for (; entry; entry = m_builtins.next(entry))
                uses |= isRetainedOnlyByReturnedIterator(m_builtins.at(entry)) ? 0 : m_builtins.at(entry).thisUses;
            return !(uses & (harmfulUses | OperandUse::MayBeResult));
        }
        case Opcode::GetByVal:
            return !aliases(user.child(1));
        case Opcode::PutByVal: {
            auto& key = typeOf(user.child(1));
            return engineClass == m_builtins.arrayClass().index && !aliases(user.child(1)) && !aliases(user.child(2)) && key.isInteger() && key.integers.isSubsetOf(Range::length());
        }
        case Opcode::CallBuiltin: {
            if (user.builtin() == m_builtins.hasOriginalStructureOperation())
                return true;
            auto& builtin = m_builtins.at(user.builtin());
            if (builtin.implementation.kind == BuiltinImplementation::Inline && builtin.implementation.inlined == InlineBuiltin::AwaitBySavingFrame)
                return !std::ranges::any_of(user.arguments.subspan(0, 2), aliases);
            if (aliases(user.child(1)) || aliases(user.child(2)) || (aliases(user.child(0)) && !builtin.hasReceiver()))
                return false;
            bool isPassedAsArgument = false;
            uint16_t uses = 0;
            for (unsigned i = 0; i < user.arguments.size(); ++i) {
                if (!aliases(user.arguments[i]))
                    continue;
                isPassedAsArgument = true;
                uses |= builtin.argumentUses(i);
            }
            if (aliases(user.child(0)))
                uses |= isRetainedOnlyByReturnedIterator(builtin) ? 0 : builtin.thisUses;
            if (uses & harmfulUses)
                return false;
            if (uses & OperandUse::MayBeResult && !aliases(sameObjectAs(id)))
                return false;
            if (!(uses & OperandUse::WritesContents) || builtin.kind == BuiltinKind::Operation)
                return true;
            return builtin.hasReceiver() && !isPassedAsArgument && user.engineClass() == engineClass;
        }
        case Opcode::Call: {
            if (aliases(user.child(0)) || aliases(user.child(1)) || !takesAtMostTwoArguments(user.child(0)))
                return false;
            return !std::ranges::any_of(user.arguments.subspan(0, std::min<size_t>(2, user.arguments.size())), aliases);
        }
        case Opcode::CallDirect: {
            if (user.constructs || user.arguments.isEmpty() || std::ranges::any_of(user.children, aliases) || std::ranges::any_of(user.arguments.subspan(1), aliases))
                return false;
            BuiltinID entry = m_facts.loweredMethods[user.function().index()];
            if (!entry)
                return false;
            auto& builtin = m_builtins.at(entry);
            auto* holder = m_builtins.classNamed(builtin.holderName);
            if (!holder || holder->index != engineClass || builtin.thisUses & (harmfulUses & ~OperandUse::EscapesToUserCode) || builtin.thisUses & OperandUse::MayBeResult)
                return false;
            return !(builtin.thisUses & OperandUse::EscapesToUserCode) || (user.arguments.size() > 1 && takesAtMostTwoArguments(user.arguments[1]));
        }
        default:
            return false;
        }
    }

    const Program& m_program;
    FunctionID m_id;
    const Function& m_function;
    const ModuleVariableFacts& m_facts;
    const Builtins& m_builtins;
    Vector<uint32_t> m_variableOf;
    BitVector m_mayBeAnotherObject;
    BitVector m_mayBeOtherPrimitive;
    BitVector m_mayBeNullish;
    Vector<uint32_t, 2> m_singleValueVariables;
    Vector<FailureRegion, 2> m_failureRegions;
};

} // anonymous namespace

unsigned callGuardedPreludeEntries(const Program& program, FunctionID id, Function& function, UnprovenReceiverPolicy unprovenReceiverPolicy)
{
    if (program.guardedPreludeEntries.isEmpty() || function.isGuardedPreludeEntry || program.isHotOutsideLoops(id) || function.kind == FunctionKind::Module)
        return 0;
    Vector<FunctionID> pendingEntries = std::exchange(function.pendingGuardedPreludeEntries, { });
    auto& builtins = Builtins::singleton();
    Vector<unsigned> numberOfPredecessors;
    numberOfPredecessors.fill(0, function.blocks.size());
    for (auto& block : function.blocks) {
        for (auto& target : block.terminator.targets)
            ++numberOfPredecessors[target.block.index()];
    }
    auto withoutCasts = [&](ValueID value) {
        while (function.at(value).opcode == Opcode::Cast)
            value = function.at(value).child(0);
        return value;
    };
    auto isRematerializedOnEachPath = [&](Opcode opcode) {
        switch (opcode) {
        case Opcode::Undefined:
        case Opcode::Null:
        case Opcode::Boolean:
        case Opcode::Integer:
        case Opcode::Double:
        case Opcode::String:
        case Opcode::FunctionReference:
        case Opcode::NewClosure:
            return true;
        default:
            return false;
        }
    };
    auto isSame = [&](ValueID left, ValueID right) {
        left = withoutCasts(left);
        right = withoutCasts(right);
        if (left == right)
            return true;
        auto& one = function.at(left);
        auto& other = function.at(right);
        if (one.opcode != other.opcode || !isRematerializedOnEachPath(one.opcode) || one.immediates != other.immediates || one.type != other.type || one.arguments.size() != other.arguments.size())
            return false;
        for (unsigned i = 0; i < one.children.size(); ++i) {
            if (!!one.children[i] != !!other.children[i] || (one.children[i] && withoutCasts(one.children[i]) != withoutCasts(other.children[i])))
                return false;
        }
        for (unsigned i = 0; i < one.arguments.size(); ++i) {
            if (withoutCasts(one.arguments[i]) != withoutCasts(other.arguments[i]))
                return false;
        }
        return true;
    };
    auto holdsEnvironment = [&](ValueID value) {
        return value && std::ranges::any_of(program.typeOf(function, value).objectLayouts, [&](LayoutID supertype) {
            return program.at(supertype).kind == LayoutKind::Environment;
        });
    };
    auto capturesEnvironment = [&](const Value& closure) {
        return std::ranges::any_of(closure.children, holdsEnvironment) || std::ranges::any_of(closure.arguments, holdsEnvironment);
    };
    auto mayBeReadFromModuleVariable = [&](ValueID value) {
        value = withoutCasts(value);
        if (function.at(value).opcode == Opcode::GetModuleVariable)
            return true;
        if (function.at(value).opcode != Opcode::Parameter)
            return false;
        if (auto& parameter = function.at(value); !parameter.parameterBlock().index() && parameter.parameterIndex() < function.blocks[0].parameters.size() && function.blocks[0].parameters[parameter.parameterIndex()] == value)
            return false;
        for (unsigned join = 1; join < function.blocks.size(); ++join) {
            size_t index = function.blocks[join].parameters.span().size();
            for (size_t i = 0; i < function.blocks[join].parameters.size(); ++i) {
                if (function.blocks[join].parameters[i] == value)
                    index = i;
            }
            if (index == function.blocks[join].parameters.size())
                continue;
            for (auto& other : function.blocks) {
                for (auto& target : other.terminator.targets) {
                    if (target.block.index() == join && function.at(withoutCasts(target.arguments[index])).opcode == Opcode::GetModuleVariable)
                        return true;
                }
            }
            return false;
        }
        return false;
    };
    Vector<unsigned> useCounts;
    auto useCountOf = [&](ValueID value) {
        if (useCounts.isEmpty()) {
            useCounts.fill(0, function.values.size());
            auto count = [&](ValueID used) {
                if (used)
                    ++useCounts[used.index()];
            };
            for (auto& other : function.blocks) {
                for (ValueID user : other.values)
                    std::as_const(function.at(user)).forEachChild(count);
                other.terminator.forEachChild(count);
            }
        }
        return useCounts[value.index()];
    };
    auto removeUsesBy = [&](auto& user) {
        if (useCounts.isEmpty())
            return;
        user.forEachChild([&](ValueID used) {
            if (used)
                --useCounts[used.index()];
        });
    };
    auto countUsesBy = [&](auto& user) {
        if (useCounts.isEmpty())
            return;
        user.forEachChild([&](ValueID used) {
            if (used)
                ++useCounts[used.index()];
        });
    };
    std::optional<BitVector> isInLoop;
    unsigned changed = 0;
    for (unsigned b = 0; b < function.blocks.size(); ++b) {
        auto& block = function.blocks[b];
        if (block.terminator.kind != TerminatorKind::Branch || block.values.isEmpty() || block.terminator.value != block.values.last())
            continue;
        auto& guard = function.at(block.terminator.value);
        if (guard.opcode != Opcode::CallBuiltin || guard.builtin() != builtins.hasOriginalStructureOperation() || guard.arguments.size() != 1)
            continue;
        auto& fastEdge = block.terminator.targets[0];
        auto& slowEdge = block.terminator.targets[1];
        if (!fastEdge.arguments.isEmpty() || !slowEdge.arguments.isEmpty() || fastEdge.block == slowEdge.block || numberOfPredecessors[fastEdge.block.index()] != 1 || numberOfPredecessors[slowEdge.block.index()] != 1)
            continue;
        auto& slow = function.at(slowEdge.block);
        auto unwindsLikeSlowPath = [&](const Terminator& terminator) {
            return terminator.kind == TerminatorKind::Invoke && slow.terminator.kind == TerminatorKind::Invoke && terminator.targets[1].block == slow.terminator.targets[1].block && std::ranges::equal(terminator.targets[1].arguments, slow.terminator.targets[1].arguments);
        };
        Vector<BlockID, 1> castsBlocks;
        BlockID fastBlock = fastEdge.block;
        while (unwindsLikeSlowPath(function.at(fastBlock).terminator) && function.at(fastBlock).terminator.targets[0].arguments.isEmpty() && numberOfPredecessors[function.at(fastBlock).terminator.targets[0].block.index()] == 1
            && std::ranges::all_of(function.at(fastBlock).values, [&](ValueID value) { return function.at(value).opcode == Opcode::Cast; })) {
            castsBlocks.append(fastBlock);
            fastBlock = function.at(fastBlock).terminator.targets[0].block;
        }
        auto& fast = function.at(fastBlock);
        if (fast.terminator.kind == TerminatorKind::Jump ? slow.terminator.kind != TerminatorKind::Jump : !unwindsLikeSlowPath(fast.terminator))
            continue;
        if (fast.terminator.targets[0].block != slow.terminator.targets[0].block || slow.values.isEmpty() || fast.values.isEmpty())
            continue;
        ValueID receiver = guard.arguments[0];
        ValueID genericCall = slow.values.last();
        ValueID directCall = fast.values.last();
        auto& generic = function.at(genericCall);
        auto& call = function.at(directCall);
        if (generic.arguments.size() > Program::maxGuardedPreludeEntryArguments || call.opcode != Opcode::CallDirect || generic.isInTailPosition)
            continue;
        bool checksResultType = generic.type != program.types.anything;
        if (checksResultType ? generic.rep != call.rep || call.type != program.at(call.function()).resultType || !program.at(generic.type).isSubsetOf(program.at(call.type), program) : call.rep != Rep::JSValue && call.rep != Rep::Cell)
            continue;
        bool isResolved = generic.opcode == Opcode::CallBuiltin;
        if (isResolved) {
            auto& builtin = builtins.at(generic.builtin());
            if (!checksResultType || builtin.kind != BuiltinKind::Method || builtin.implementation.kind != BuiltinImplementation::Prelude || program.preludeFunctions[static_cast<unsigned>(builtin.implementation.prelude)] != call.function())
                continue;
            if (generic.child(0) != receiver || generic.child(1) || generic.child(2))
                continue;
        }
        ValueID read;
        if (generic.opcode == Opcode::Call) {
            read = generic.child(0);
            if (function.at(read).opcode != Opcode::GetById || function.at(read).child(0) != receiver || generic.child(1) != receiver)
                continue;
            bool isFound = false;
            bool isUnobservable = true;
            for (unsigned i = block.values.size() - 1; i-- && !isFound && isUnobservable;) {
                isFound = block.values[i] == read;
                switch (function.at(block.values[i]).opcode) {
                case Opcode::Undefined:
                case Opcode::Null:
                case Opcode::Boolean:
                case Opcode::Integer:
                case Opcode::Double:
                case Opcode::String:
                case Opcode::BigInt:
                case Opcode::FunctionReference:
                case Opcode::NewClosure:
                    break;
                default:
                    isUnobservable = isFound;
                }
            }
            if (!isFound || useCountOf(read) != 1)
                continue;
        } else if (!isResolved && (generic.opcode != Opcode::CallMethod || generic.child(0) != receiver))
            continue;
        if (call.arguments.size() <= generic.arguments.size() || withoutCasts(call.arguments[0]) != withoutCasts(receiver))
            continue;
        bool matches = true;
        std::optional<unsigned> copiedArgumentCaptureCount;
        for (unsigned i = 0; i < generic.arguments.size(); ++i) {
            ValueID argument = generic.arguments[i];
            if (function.at(argument).opcode != Opcode::NewClosure)
                continue;
            unsigned usesByBothCalls = 1 + (i + 1 < call.arguments.size() && call.arguments[i + 1] == argument);
            if (useCountOf(argument) != usesByBothCalls)
                continue;
            matches &= !i && !capturesEnvironment(function.at(argument)) && program.usesSlimClosures(program.at(function.at(argument).function()));
            copiedArgumentCaptureCount = function.at(argument).arguments.size();
        }
        FunctionID entry = program.guardedPreludeEntries.get(Program::guardedPreludeEntryKey(call.function(), generic.arguments.size(), matches ? copiedArgumentCaptureCount : std::nullopt, checksResultType));
        if (!entry || !matches)
            continue;
        for (unsigned i = 0; i + 1 < fast.values.size(); ++i) {
            Opcode opcode = function.at(fast.values[i]).opcode;
            matches &= opcode == Opcode::Cast || isRematerializedOnEachPath(opcode);
        }
        for (unsigned i = 0; i + 1 < slow.values.size(); ++i)
            matches &= isRematerializedOnEachPath(function.at(slow.values[i]).opcode);
        for (unsigned i = 1; i < call.arguments.size(); ++i) {
            if (i <= generic.arguments.size())
                matches &= isSame(call.arguments[i], generic.arguments[i - 1]);
            else
                matches &= function.at(call.arguments[i]).opcode == Opcode::Undefined;
        }
        auto& fastArguments = fast.terminator.targets[0].arguments;
        auto& slowArguments = slow.terminator.targets[0].arguments;
        matches &= fastArguments.size() == slowArguments.size();
        for (unsigned i = 0; matches && i < fastArguments.size(); ++i)
            matches = fastArguments[i] == slowArguments[i] ? fastArguments[i] != directCall : fastArguments[i] == directCall && slowArguments[i] == genericCall;
        if (!matches)
            continue;
        if (!isInLoop)
            isInLoop = blocksInLoops(function);
        if (isInLoop->get(b))
            continue;
        Opcode receiverDefinition = function.at(withoutCasts(receiver)).opcode;
        if (unprovenReceiverPolicy == UnprovenReceiverPolicy::Wait && (receiverDefinition == Opcode::NewArray || receiverDefinition == Opcode::CreateRest || receiverDefinition == Opcode::CallBuiltin || receiverDefinition == Opcode::CallDirect || mayBeReadFromModuleVariable(receiver))) {
            if (!function.pendingGuardedPreludeEntries.contains(entry))
                function.pendingGuardedPreludeEntries.append(entry);
            continue;
        }
        if (unprovenReceiverPolicy == UnprovenReceiverPolicy::Speculate && !pendingEntries.contains(entry))
            continue;
        if (read) {
            Value undefinedValue;
            undefinedValue.opcode = Opcode::Undefined;
            undefinedValue.position = function.at(read).position;
            undefinedValue.inlineFrame = function.at(read).inlineFrame;
            undefinedValue.type = program.types.undefined;
            undefinedValue.rep = repOf(program.at(undefinedValue.type));
            removeUsesBy(std::as_const(function.at(read)));
            function.at(read) = WTF::move(undefinedValue);
        }
        Value entryCall;
        entryCall.opcode = Opcode::CallDirect;
        entryCall.position = generic.position;
        entryCall.inlineFrame = generic.inlineFrame;
        entryCall.isInTailPosition = generic.isInTailPosition;
        entryCall.type = checksResultType ? call.type : generic.type;
        entryCall.rep = generic.rep;
        entryCall.setPrediction(generic.prediction());
        entryCall.immediates = { entry.index(), Format::none };
        entryCall.arguments.append(receiver);
        entryCall.arguments.appendVector(generic.arguments);
        removeUsesBy(std::as_const(generic));
        generic = WTF::move(entryCall);
        countUsesBy(std::as_const(generic));
        removeUsesBy(std::as_const(function.at(block.values.last())));
        block.values.removeLast();
        block.values.appendVector(slow.values);
        removeUsesBy(block.terminator);
        block.terminator = WTF::move(slow.terminator);
        slow.values.clear();
        slow.terminator = Terminator { };
        if (unprovenReceiverPolicy == UnprovenReceiverPolicy::Wait) {
            auto empty = [&](Block& left) {
                for (ValueID value : left.values)
                    removeUsesBy(std::as_const(function.at(value)));
                removeUsesBy(left.terminator);
                left.values.clear();
                left.terminator = Terminator { };
            };
            empty(fast);
            for (BlockID castsBlock : castsBlocks)
                empty(function.at(castsBlock));
        }
        ++changed;
    }
    if (changed && unprovenReceiverPolicy == UnprovenReceiverPolicy::Wait)
        cleanUp(program, function);
    else if (changed)
        ReceiverGuardElimination(program, function).removeUnreachableBlocks();
    return changed;
}

unsigned resolvePreludeCalls(const Program& program, Function& function, CleanUpTiming timing)
{
    return BuiltinSpecialization(program, function, BuiltinSpecialization::Mode::Prelude).run(timing);
}

unsigned speculateOnReceivers(const Program& program, FunctionID id, Function& function)
{
    BuiltinSpecialization speculation(program, function, BuiltinSpecialization::Mode::Speculation);
    if (!program.isHotOutsideLoops(id) && function.kind != FunctionKind::Module)
        speculation.useStubsInColdCode();
    return speculation.run();
}

void compareHeaderNamesWithPassedConstants(Program& program)
{
    constexpr unsigned maxConstants = 64;
    constexpr unsigned maxConstantsComparedInTurn = 4;
    constexpr unsigned maxRounds = 4;
    auto& functions = std::as_const(program).functions;
    unsigned count = functions.size();
    static constexpr OpcodeSet builtinCalls { Opcode::CallBuiltin };
    static constexpr OpcodeSet calls { Opcode::CallDirect, Opcode::CallMethod, Opcode::Call };

    auto entryParameterOf = [&](const Function& function, ValueID id) -> std::optional<unsigned> {
        id = program.originOf(function, id);
        if (function.at(id).opcode != Opcode::Parameter)
            return std::nullopt;
        auto parameters = function.blocks[0].parameters.span();
        size_t index = std::ranges::find(parameters, id) - parameters.begin();
        return index == parameters.size() ? std::nullopt : std::optional(static_cast<unsigned>(index));
    };
    auto numberOperandOf = [&](const Function& function, const Value& call) {
        ValueID number = call.arguments.last();
        while (function.at(number).opcode == Opcode::Cast)
            number = function.at(number).child(0);
        return number;
    };

    struct Use {
        ValueID call;
        unsigned parameter;
    };
    Vector<Vector<Use, 1>> uses(count);
    forEachIndexInParallel(count, [&](unsigned i) {
        auto& function = functions[i];
        if (function.blocks.isEmpty())
            return;
        functions.forEachIndexedValue(i, builtinCalls, [&](ValueID id) {
            auto& call = function.at(id);
            if (call.builtin() != Operations::CallHeadersMethod)
                return;
            auto& number = function.at(numberOperandOf(function, call));
            if (number.opcode != Opcode::Integer || (number.integer() & ((1 << headersMethodShift) - 1)) != unknownHeaderName || static_cast<HeadersMethod>(number.integer() >> headersMethodShift) == HeadersMethod::Get)
                return;
            if (auto parameter = entryParameterOf(function, call.arguments[1]))
                uses[i].append({ id, *parameter });
        });
    });

    struct Carrier {
        unsigned function;
        unsigned parameter;
        Vector<StringID, 8> constants;
        Vector<unsigned, 2> handsOnTo;
        bool hasTooMany { false };
    };
    Vector<Carrier> carriers;
    auto carrierOf = [&](unsigned function, unsigned parameter) -> std::optional<unsigned> {
        size_t index = carriers.findIf([&](auto& carrier) { return carrier.function == function && carrier.parameter == parameter; });
        return index == notFound ? std::nullopt : std::optional(static_cast<unsigned>(index));
    };
    for (unsigned i = 0; i < count; ++i) {
        for (auto& use : uses[i]) {
            if (!carrierOf(i, use.parameter))
                carriers.append({ i, use.parameter, { }, { }, false });
        }
    }
    if (carriers.isEmpty())
        return;

    struct PassedValue {
        unsigned carrier;
        StringID constant;
        unsigned fromFunction;
        unsigned fromParameter;
    };
    unsigned firstNewCarrier = 0;
    for (unsigned round = 0; round < maxRounds && firstNewCarrier < carriers.size(); ++round) {
        unsigned end = carriers.size();
        Vector<Vector<PassedValue>> found(count);
        forEachIndexInParallel(count, [&](unsigned i) {
            auto& function = functions[i];
            if (function.blocks.isEmpty())
                return;
            functions.forEachIndexedValue(i, calls, [&](ValueID id) {
                auto& call = function.at(id);
                StringID calledName;
                if (call.opcode == Opcode::CallMethod)
                    calledName = call.string();
                else if (call.opcode == Opcode::Call) {
                    auto& callee = function.at(program.originOf(function, call.child(0)));
                    if (callee.opcode != Opcode::GetById)
                        return;
                    calledName = callee.string();
                }
                for (unsigned c = firstNewCarrier; c < end; ++c) {
                    auto& callee = functions[carriers[c].function];
                    unsigned position = carriers[c].parameter;
                    if (call.opcode == Opcode::CallDirect) {
                        if (call.function().index() != carriers[c].function)
                            continue;
                    } else {
                        unsigned first = callee.captureTypes.size() + callee.has(Format::FunctionHasThis);
                        if (callee.name != calledName || callee.isGenericEntrypoint() || callee.has(Format::FunctionIsHiddenInStackTraces) || position < first)
                            continue;
                        position -= first;
                    }
                    if (position >= call.arguments.size())
                        continue;
                    ValueID argument = program.originOf(function, call.arguments[position]);
                    if (function.at(argument).opcode == Opcode::String)
                        found[i].append({ c, function.at(argument).string(), 0, UINT_MAX });
                    else if (auto parameter = entryParameterOf(function, argument))
                        found[i].append({ c, StringID(), i, *parameter });
                }
            });
        });
        firstNewCarrier = end;
        for (auto& list : found) {
            for (auto& passed : list) {
                if (passed.fromParameter == UINT_MAX) {
                    auto& carrier = carriers[passed.carrier];
                    if (carrier.constants.contains(passed.constant) || knownHeaderNameOf(program.string(passed.constant)) == unknownHeaderName)
                        continue;
                    if (carrier.constants.size() == maxConstants)
                        carrier.hasTooMany = true;
                    else
                        carrier.constants.append(passed.constant);
                    continue;
                }
                auto from = carrierOf(passed.fromFunction, passed.fromParameter);
                if (!from) {
                    from = carriers.size();
                    carriers.append({ passed.fromFunction, passed.fromParameter, { }, { }, false });
                }
                if (*from != passed.carrier && !carriers[*from].handsOnTo.contains(passed.carrier))
                    carriers[*from].handsOnTo.append(passed.carrier);
            }
        }
    }
    for (unsigned round = 0; round < maxRounds; ++round) {
        for (auto& from : carriers) {
            for (unsigned to : from.handsOnTo) {
                auto& carrier = carriers[to];
                carrier.hasTooMany |= from.hasTooMany;
                for (StringID constant : from.constants) {
                    if (carrier.constants.contains(constant))
                        continue;
                    if (carrier.constants.size() == maxConstants)
                        carrier.hasTooMany = true;
                    else
                        carrier.constants.append(constant);
                }
            }
        }
    }

    unsigned comparedCalls = 0;
    for (unsigned i = 0; i < count; ++i) {
        for (auto& use : uses[i]) {
            auto& carrier = carriers[*carrierOf(i, use.parameter)];
            if (carrier.constants.isEmpty() || carrier.hasTooMany)
                continue;
            auto& function = program.functions[i];
            unsigned block = 0;
            size_t index = notFound;
            for (; block < function.blocks.size(); ++block) {
                index = function.blocks[block].values.find(use.call);
                if (index != notFound)
                    break;
            }
            RELEASE_ASSERT(index != notFound);
            Value call = function.at(use.call);
            ValueID replaced = numberOperandOf(function, call);
            int64_t unknown = function.at(replaced).integer();
            auto add = [&](Opcode opcode, TypeID type) {
                Value value;
                value.opcode = opcode;
                value.position = call.position;
                value.inlineFrame = call.inlineFrame;
                value.type = type;
                value.rep = repOf(program.at(type));
                function.values.append(WTF::move(value));
                return ValueID(function.values.size() - 1);
            };
            auto addBlock = [&] {
                function.blocks.append(Block { });
                return function.blocks.size() - 1;
            };
            unsigned after = addBlock();
            function.blocks[after].values.append(function.blocks[block].values.subspan(index));
            function.blocks[block].values.shrink(index);
            if (replaced == call.arguments.last())
                function.blocks[block].values.removeFirst(replaced);
            function.blocks[after].terminator = std::exchange(function.blocks[block].terminator, Terminator { });
            ValueID number = add(Opcode::Parameter, program.types.int32);
            function.at(number).immediates = { after, 0 };
            function.blocks[after].parameters.append(number);
            function.at(use.call).arguments.last() = number;
            auto jumpWith = [&](unsigned from, int64_t bits) {
                ValueID constant = add(Opcode::Integer, program.types.int32);
                function.at(constant).setBits(bits);
                function.blocks[from].values.append(constant);
                function.blocks[from].terminator.kind = TerminatorKind::Jump;
                function.blocks[from].terminator.targets.append(Target { BlockID(after), Arguments { constant } });
            };
            auto branch = [&](unsigned from, ValueID condition, unsigned then, unsigned otherwise) {
                auto& test = function.blocks[from].terminator;
                test.kind = TerminatorKind::Branch;
                test.value = condition;
                test.targets.append(Target { BlockID(then), { } });
                test.targets.append(Target { BlockID(otherwise), { } });
            };
            unsigned isNoneOfThem = addBlock();
            jumpWith(isNoneOfThem, unknown);
            auto compareInTurn = [&](unsigned current, ValueID name, std::span<const StringID> constants) {
                for (StringID constant : constants) {
                    ValueID text = add(Opcode::String, program.types.string);
                    function.at(text).immediates[0] = constant.index();
                    ValueID isSame = add(Opcode::StrictEq, program.types.boolean);
                    function.at(isSame).children = { name, text, ValueID() };
                    function.blocks[current].values.append(text);
                    function.blocks[current].values.append(isSame);
                    unsigned same = addBlock();
                    unsigned next = addBlock();
                    branch(current, isSame, same, next);
                    jumpWith(same, (unknown & ~static_cast<int64_t>((1 << headersMethodShift) - 1)) | knownHeaderNameOf(program.string(constant)));
                    current = next;
                }
                function.blocks[current].terminator.kind = TerminatorKind::Jump;
                function.blocks[current].terminator.targets.append(Target { BlockID(isNoneOfThem), { } });
            };
            ++comparedCalls;
            ValueID name = call.arguments[1];
            if (carrier.constants.size() <= maxConstantsComparedInTurn) {
                compareInTurn(block, name, carrier.constants.span());
                continue;
            }
            unsigned isString = block;
            if (!program.castIsRedundant(program.at(function.at(name).type), program.at(program.types.string), CastReason::Claim)) {
                ValueID test = add(Opcode::TypeOfIs, program.types.boolean);
                function.at(test).children[0] = name;
                function.at(test).immediates[0] = static_cast<uint32_t>(TypeofType::String);
                function.blocks[block].values.append(test);
                isString = addBlock();
                branch(block, test, isString, isNoneOfThem);
                ValueID text = add(Opcode::Cast, program.types.string);
                function.at(text).children[0] = name;
                function.at(text).immediates[1] = static_cast<uint32_t>(CastReason::Claim);
                function.blocks[isString].values.append(text);
                name = text;
            }
            auto& builtins = Builtins::singleton();
            BuiltinID lengthGetter = builtins.findForInstance(BuiltinKind::Getter, builtins.stringClass(), builtins.nameOf(asBytes("length"_s.span8())));
            RELEASE_ASSERT(lengthGetter);
            TypeID lengthType = program.builtinIntegerResultTypes[lengthGetter.index()];
            ValueID length = add(Opcode::CallBuiltin, lengthType ? lengthType : program.types.int32);
            function.at(length).children[0] = name;
            function.at(length).immediates = { lengthGetter.index(), builtins.stringClass().index };
            function.blocks[isString].values.append(length);
            Vector<StringID, 8> sorted = carrier.constants;
            std::ranges::stable_sort(sorted, { }, [&](StringID constant) {
                return program.string(constant).size();
            });
            function.blocks[isString].terminator.kind = TerminatorKind::Switch;
            function.blocks[isString].terminator.value = length;
            for (size_t first = 0; first < sorted.size();) {
                size_t size = program.string(sorted[first]).size();
                size_t end = first;
                while (end < sorted.size() && program.string(sorted[end]).size() == size)
                    ++end;
                unsigned hasThisLength = addBlock();
                function.blocks[isString].terminator.cases.append(size);
                function.blocks[isString].terminator.targets.append(Target { BlockID(hasThisLength), { } });
                compareInTurn(hasThisLength, name, sorted.span().subspan(first, end - first));
                first = end;
            }
            function.blocks[isString].terminator.targets.append(Target { BlockID(isNoneOfThem), { } });
        }
    }
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: ", comparedCalls, " header calls compare a name that is a parameter with the constants that calls pass");
}

unsigned eliminateGuardsOnUnescapedReceivers(const Program& program, Function& function)
{
    return ReceiverGuardElimination(program, function).run();
}

void eliminateGuardsOnFreshResults(Program& program)
{
    unsigned count = program.functions.size();
    program.freshResultClasses.fill(UINT16_MAX, count);
    static constexpr OpcodeSet directCalls { Opcode::CallDirect };
    auto callsFunctionIn = [&](unsigned i, const auto& isMatching) {
        bool result = false;
        std::as_const(program).functions.forEachValue(i, directCalls, [&](ValueID id) {
            unsigned callee = std::as_const(program).functions[i].at(id).function().index();
            result |= callee < count && isMatching(callee);
        });
        return result;
    };
    Vector<uint16_t> classesBeforeLastRound;
    for (unsigned round = 0; round < 4; ++round) {
        Vector<uint16_t> found = program.freshResultClasses;
        forEachIndexInParallel(count, [&](unsigned i) {
            if (found[i] != UINT16_MAX || !program.functions.mayEndWith(i, TerminatorKind::Return))
                return;
            if (round && !callsFunctionIn(i, [&](unsigned callee) { return program.freshResultClasses[callee] != classesBeforeLastRound[callee]; }))
                return;
            auto& function = program.functions.atKeepingValueIndex(i);
            if (function.blocks.isEmpty() || function.declaredAs || !function.resultTypes.isEmpty() || function.flags & (FunctionIsPartOfHiddenCoroutine | FunctionIsPartOfAsyncFunction))
                return;
            if (function.kind != FunctionKind::Plain && function.kind != FunctionKind::Arrow && function.kind != FunctionKind::Method)
                return;
            if (uint32_t engineClass = ReceiverGuardElimination(program, function).freshResultClass(); engineClass != Format::none)
                found[i] = static_cast<uint16_t>(engineClass);
        });
        if (found == program.freshResultClasses)
            break;
        classesBeforeLastRound = std::exchange(program.freshResultClasses, WTF::move(found));
    }
    forEachIndexInParallel(count, [&](unsigned i) {
        if (!callsFunctionIn(i, [&](unsigned callee) { return program.freshResultClasses[callee] != UINT16_MAX; }) || std::as_const(program).functions[i].blocks.isEmpty())
            return;
        ReceiverGuardElimination guards(program, program.functions.atKeepingValueIndex(i));
        if (guards.run()) {
            program.functions.invalidateValueIndex(i);
            guards.removeUnreachableBlocks();
        }
    });
}

void eliminateGuardsOnUnescapedModuleVariables(Program& program)
{
    auto& builtins = Builtins::singleton();
    auto& functions = std::as_const(program).functions;
    unsigned count = functions.size();
    ModuleVariableFacts facts;
    facts.classes.fill(ModuleVariableFacts::isNeverStored, program.variables.size());
    facts.returnedVariables.fill(ModuleVariableFacts::none, count);
    static constexpr OpcodeSet stores { Opcode::PutModuleVariable };
    static constexpr uint16_t isNil = ModuleVariableFacts::isNeverStored;
    Vector<unsigned> functionsWithStores;
    for (unsigned i = 0; i < count; ++i) {
        if (!functions[i].blocks.isEmpty() && functions.mayContain(i, stores))
            functionsWithStores.append(i);
    }
    Vector<Vector<std::pair<uint32_t, uint16_t>>> storedValueOf(functionsWithStores.size());
    forEachIndexInParallel(functionsWithStores.size(), [&](unsigned functionIndex) {
        unsigned i = functionsWithStores[functionIndex];
        auto& function = functions[i];
        auto& found = storedValueOf[functionIndex];
        ReceiverGuardElimination allocations(program, program.functions.atKeepingValueIndex(i));
        Vector<std::pair<ValueID, uint32_t>, 4> stored;
        functions.forEachIndexedValue(i, stores, [&](ValueID id) {
            auto& store = function.at(id);
            uint32_t variable = store.variable().index();
            if (auto& type = program.typeOf(function, store.child(0)); !type.hasObjects()) {
                found.append({ variable, type.hasOnlyPrimitives(Primitive::UndefinedOrNull) ? isNil : ModuleVariableFacts::escapes });
                return;
            }
            uint32_t engineClass = allocations.allocationClass(store.child(0));
            if (engineClass == Format::none || !builtins.classes()[engineClass].canCheckOriginalStructure()) {
                found.append({ variable, ModuleVariableFacts::escapes });
                return;
            }
            found.append({ variable, static_cast<uint16_t>(engineClass) });
            stored.append({ allocations.allocationOf(store.child(0)), variable });
        });
        if (function.kind != FunctionKind::Module || stored.isEmpty())
            return;
        uint32_t returned = ModuleVariableFacts::none;
        bool returnsOneVariable = true;
        for (auto& block : function.blocks) {
            if (block.terminator.kind != TerminatorKind::Return)
                continue;
            uint32_t variable = ModuleVariableFacts::none;
            for (auto& [allocation, storedVariable] : stored) {
                if (block.terminator.value && allocation == allocations.allocationOf(block.terminator.value))
                    variable = storedVariable;
            }
            returnsOneVariable &= variable != ModuleVariableFacts::none && (returned == ModuleVariableFacts::none || returned == variable);
            returned = variable;
        }
        if (returnsOneVariable)
            facts.returnedVariables[i] = returned;
    });
    for (auto& found : storedValueOf) {
        for (auto [variable, engineClass] : found) {
            uint16_t& known = facts.classes[variable];
            if (engineClass == isNil)
                facts.mayHoldNullish.set(variable);
            else if (known == ModuleVariableFacts::isNeverStored || known == engineClass)
                known = engineClass;
            else
                known = ModuleVariableFacts::escapes;
        }
    }
    if (!facts.classes.containsIf([](uint16_t engineClass) { return engineClass < ModuleVariableFacts::isNeverStored; }))
        return;
    facts.loweredMethods.fill(BuiltinID(), count);
    for (unsigned i = 0; i < program.preludeFunctions.size(); ++i) {
        BuiltinID entry = builtins.entryImplementedBy(static_cast<PreludeFunction>(i));
        if (program.preludeFunctions[i] && entry && builtins.at(entry).kind == BuiltinKind::Method)
            facts.loweredMethods[program.preludeFunctions[i].index()] = entry;
    }
    for (FunctionID prelude : program.preludeFunctions) {
        if (!prelude || !facts.loweredMethods[prelude.index()] || functions[prelude.index()].blocks.isEmpty() || functions[prelude.index()].blocks[0].parameters.size() < 2)
            continue;
        auto& function = functions[prelude.index()];
        for (auto& value : function.values) {
            if (value.opcode != Opcode::CallDirect || value.arguments.size() < 2 || functions[value.function().index()].module != function.module || facts.loweredMethods[value.function().index()])
                continue;
            if (program.originOf(function, value.arguments[0]) == function.blocks[0].parameters[0] && program.originOf(function, value.arguments[1]) == function.blocks[0].parameters[1])
                facts.loweredMethods[value.function().index()] = facts.loweredMethods[prelude.index()];
        }
    }
    for (FunctionID guardedEntry : program.guardedPreludeEntries.values()) {
        for (auto& value : functions[guardedEntry.index()].values) {
            if (value.opcode == Opcode::CallDirect && !functions[value.function().index()].isGuardedPreludeEntry && facts.loweredMethods[value.function().index()])
                facts.loweredMethods[guardedEntry.index()] = facts.loweredMethods[value.function().index()];
        }
    }
    Vector<uint8_t> escapes(FillWith { }, program.variables.size(), static_cast<uint8_t>(0));
    Vector<uint8_t> holdsAliases(FillWith { }, count, static_cast<uint8_t>(0));
    forEachIndexInParallel(count, [&](unsigned i) {
        if (functions[i].blocks.isEmpty())
            return;
        ModuleVariableAliases aliases(program, FunctionID(i), facts);
        if (!aliases.find())
            return;
        holdsAliases[i] = 1;
        aliases.forEachEscape([&](uint32_t variable) {
            WTF::atomicStore(&escapes[variable], static_cast<uint8_t>(1), std::memory_order_relaxed);
        });
    });
    unsigned numberOfVariables = 0;
    for (unsigned i = 0; i < facts.classes.size(); ++i) {
        if (escapes[i] || !facts.isCandidate(i))
            facts.classes[i] = ModuleVariableFacts::escapes;
        else
            ++numberOfVariables;
    }
    std::atomic<unsigned> eliminatedCount { 0 };
    std::atomic<unsigned> numberOfCalls { 0 };
    forEachIndexInParallel(numberOfVariables ? count : 0, [&](unsigned i) {
        if (!holdsAliases[i])
            return;
        ModuleVariableAliases aliases(program, FunctionID(i), facts);
        if (!aliases.find())
            return;
        auto [eliminated, calls] = aliases.eliminate(program.functions.atKeepingValueIndex(i));
        numberOfCalls += calls;
        if (!eliminated)
            return;
        eliminatedCount += eliminated;
        ReceiverGuardElimination(program, program.functions[i]).removeUnreachableBlocks();
    });
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: ", numberOfVariables, " module variables hold objects that only built-in operations observe: ", eliminatedCount.load(), " guards and method reads eliminated, ", numberOfCalls.load(), " calls of built-in methods without a generic fallback");
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
