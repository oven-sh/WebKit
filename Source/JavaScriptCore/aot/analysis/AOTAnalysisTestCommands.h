/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTAnalysisDump.h"
#include "AOTEffectSummaries.h"
#include "AOTFlowResults.h"
#include "AOTIRBuilder.h"
#include "AOTSupportTestCommands.h"
#include "AOTTestCommand.h"
#include "AOTTypeArena.h"
#include "AOTVector.h"
#include <optional>
#include <type_traits>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/OptionSet.h>

namespace JSC { namespace AOT { namespace Typed {

void runAnalyses(TestCommandContext&);
void runGuardAvailability(TestCommandContext&);
void runFailedTests(TestCommandContext&);
void runInductionVariables(TestCommandContext&);
void runBlockFrequencies(TestCommandContext&);
void runForwardDataflow(TestCommandContext&);

template<typename Parser>
using ParsedType = typename std::invoke_result_t<Parser, StringView>::value_type;

inline std::optional<uint32_t> parseIndexBelow(StringView text, size_t limit)
{
    std::optional<uint32_t> index = parseTestNumber<uint32_t>(text);
    return index && *index < limit ? index : std::nullopt;
}

template<typename IDType>
auto idParser(char prefix, size_t tableSize)
{
    return [=](StringView text) {
        std::optional<uint32_t> index = text.startsWith(prefix) ? parseIndexBelow(text.substring(1), tableSize) : std::nullopt;
        return index ? std::optional(IDType(*index)) : std::nullopt;
    };
}

auto filterParser(auto parse, auto isAccepted)
{
    return [=](StringView text) {
        auto result = parse(text);
        return result && isAccepted(*result) ? result : std::nullopt;
    };
}

auto pairParser(char separator, auto parseFirst, auto parseSecond)
{
    using Pair = std::pair<ParsedType<decltype(parseFirst)>, ParsedType<decltype(parseSecond)>>;
    return [=](StringView text) -> std::optional<Pair> {
        size_t separatorPosition = text.find(separator);
        if (separatorPosition == notFound)
            return std::nullopt;
        auto first = parseFirst(text.left(separatorPosition));
        auto second = parseSecond(text.substring(separatorPosition + 1));
        if (!first || !second)
            return std::nullopt;
        return Pair { *first, *second };
    };
}

inline void addTo(BitVector& set, uint32_t index) { set.set(index); }
template<typename Tag> void addTo(BitVector& set, ID<Tag> id) { set.set(id.index()); }
template<typename Flag> void addTo(OptionSet<Flag>& set, Flag flag) { set.add(flag); }
template<typename T> void addTo(Vector<T>& list, T item) { list.append(item); }

template<typename Collection>
auto collectionParser(auto parseItem)
{
    return [=](StringView text) -> std::optional<Collection> {
        Collection collection;
        for (StringView itemText : text.splitAllowingEmptyEntries(',')) {
            auto item = parseItem(itemText);
            if (!item)
                return std::nullopt;
            addTo(collection, *item);
        }
        return collection;
    };
}

inline size_t numberOfTargets(const Block& block) { return block.terminator.targets.size(); }
inline size_t numberOfInsertionPoints(const Block& block) { return block.values.size() + 1; }

auto listParser(auto parseItem) { return collectionParser<Vector<ParsedType<decltype(parseItem)>>>(parseItem); }
inline auto indexParser(size_t limit) { return [limit](StringView text) { return parseIndexBelow(text, limit); }; }
inline auto blockParser(const Function& function) { return idParser<BlockID>('b', function.blocks.size()); }
inline auto edgeTargetParser(const Function& function) { return filterParser(blockParser(function), [](BlockID block) { return block != entryBlock; }); }

inline auto valueParser(const Function& function)
{
    return filterParser(idParser<ValueID>('v', function.values.size()), [&function](ValueID value) { return !!function.at(value).type; });
}

inline auto functionParser(const Program& program)
{
    return filterParser(idParser<FunctionID>('f', program.functions.size()), [&program](FunctionID id) { return program.at(id).hasBody(); });
}

template<typename Result>
auto blockAndIndexParser(const Function& function, char separator, size_t (*numberOfIndexes)(const Block&))
{
    return [=, &function](StringView text) -> std::optional<Result> {
        auto parts = pairParser(separator, blockParser(function), indexParser(UINT32_MAX))(text);
        if (!parts || parts->second >= numberOfIndexes(function.at(parts->first)))
            return std::nullopt;
        return Result { parts->first, parts->second };
    };
}

inline auto edgeParser(const Function& function)
{
    return [&function](StringView text) -> std::optional<Edge> {
        constexpr ASCIILiteral handlerSuffix = ".handler"_s;
        if (!text.endsWith(handlerSuffix))
            return blockAndIndexParser<Edge>(function, '.', numberOfTargets)(text);
        std::optional<BlockID> block = blockParser(function)(text.left(text.length() - handlerSuffix.length()));
        if (!block || !function.at(*block).handler)
            return std::nullopt;
        return Edge::toHandlerOf(*block);
    };
}

inline auto insertionPointParser(const Function& function) { return blockAndIndexParser<ProgramPoint>(function, ':', numberOfInsertionPoints); }

template<typename IDType>
Vector<IDType> idsMatching(size_t tableSize, NOESCAPE const Invocable<bool(IDType)> auto& matches)
{
    return Vector<IDType>(tableSize, [&](size_t index) {
        return matches(IDType(index)) ? std::optional(IDType(index)) : std::nullopt;
    });
}

template<typename IDType>
Vector<IDType> idsIn(const BitVector& set)
{
    Vector<IDType> ids;
    for (size_t index : set)
        ids.append(IDType(index));
    return ids;
}

inline void dumpItem(PrintStream& out, ASCIILiteral text) { out.print(text); }
inline void dumpItem(PrintStream& out, uint32_t number) { out.print(number); }
inline void dumpItem(PrintStream& out, bool answer) { dumpBoolean(out, answer); }
inline void dumpItem(PrintStream& out, ValueID id) { dumpID(out, id); }
inline void dumpItem(PrintStream& out, BlockID id) { dumpID(out, id); }
inline void dumpItem(PrintStream& out, ObjectID id) { dumpID(out, id); }
inline void dumpItem(PrintStream& out, Edge edge) { dumpEdge(out, edge); }
inline void dumpItem(PrintStream& out, ProgramPoint point) { dumpPoint(out, point); }
inline void dumpItem(PrintStream& out, const Use& use) { dumpUse(out, use); }
inline void dumpItem(PrintStream& out, OperandUses uses) { dumpOperandUses(out, uses); }
inline void dumpItem(PrintStream& out, const Vector<ValueID>& values) { dumpValues(out, values.span()); }
inline void dumpItem(PrintStream& out, const Vector<BlockID>& blocks) { dumpBlocks(out, blocks.span()); }
inline void dumpItem(PrintStream& out, const Vector<ObjectID>& objects) { dumpObjects(out, objects.span()); }

template<typename... Items>
void dumpLine(PrintStream& out, const Items&... items)
{
    (dumpItem(out, items), ...);
    out.print("\n");
}

class CommandWords {
public:
    explicit CommandWords(TestCommandContext& context)
        : m_out(context.out)
        , m_unreadWords(context.arguments)
    {
    }

    bool hasFailed() const { return m_hasFailed; }

    void fail(StringView word)
    {
        if (!std::exchange(m_hasFailed, true))
            printBadArgument(m_out, word);
    }

    void failMissingArgument()
    {
        if (!std::exchange(m_hasFailed, true))
            printMissingArgument(m_out);
    }

    bool finish()
    {
        if (!m_unreadWords.isEmpty())
            fail(m_unreadWords.first());
        return !m_hasFailed;
    }

    bool parseFlag(ASCIILiteral flag) { return m_unreadWords.removeAll(flag); }

    template<typename Parser>
    Vector<ParsedType<Parser>> parseAll(ASCIILiteral key, const Parser& parse)
    {
        Vector<ParsedType<Parser>> results;
        m_unreadWords.removeAllMatching([&](StringView word) {
            if (word.find('=') != key.length() || !word.startsWith(key))
                return false;
            if (auto result = parse(word.substring(key.length() + 1)))
                results.append(WTF::move(*result));
            else
                fail(word);
            return true;
        });
        return results;
    }

    template<typename Parser>
    Vector<ParsedType<Parser>> parseRemaining(const Parser& parse)
    {
        Vector<ParsedType<Parser>> results;
        for (StringView word : std::exchange(m_unreadWords, { })) {
            if (auto result = parse(word))
                results.append(WTF::move(*result));
            else
                fail(word);
        }
        return results;
    }

    template<typename Parser>
    std::optional<ParsedType<Parser>> parseOptional(ASCIILiteral key, const Parser& parse)
    {
        auto results = parseAll(key, parse);
        if (results.size() > 1)
            fail(key);
        if (results.isEmpty())
            return std::nullopt;
        return results.takeLast();
    }

    template<typename Parser>
    ParsedType<Parser> parseRequired(ASCIILiteral key, const Parser& parse)
    {
        auto result = parseOptional(key, parse);
        if (!result)
            failMissingArgument();
        return WTF::move(result).value_or(ParsedType<Parser> { });
    }

private:
    PrintStream& m_out;
    Vector<String> m_unreadWords;
    bool m_hasFailed { false };
};

struct FlowPreparation {
    const FlowResults* flowOrNull() const LIFETIME_BOUND { return flow ? &*flow : nullptr; }
    const EffectSummaries* effectsOrNull() const LIFETIME_BOUND { return effects ? &*effects : nullptr; }

    std::optional<FlowResults> flow;
    std::optional<EffectSummaries> effects;
};

FlowPreparation prepareFlow(TestCommandContext&, CommandWords&);

struct Session {
    Session(TestCommandContext& context, FunctionID id, const FlowResults* flow = nullptr, const EffectSummaries* effects = nullptr)
        : id(id)
        , function(context.program->at(id))
        , types(context.program->types)
        , analysisContext { context.build, *context.program, flow, effects, id }
        , analyses(analysisContext, function)
        , builder(analysisContext, function, types, &analyses)
    {
    }

    FunctionID id;
    Function& function;
    TypeArena types;
    AnalysisContext analysisContext;
    FunctionAnalyses analyses;
    IRBuilder builder;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
