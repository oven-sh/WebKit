/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTEmbedConstantCollections.h"

#if ENABLE(AOT)

#include "AOTAnalysisDump.h"
#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
#include "AOTEffectSummaries.h"
#include "AOTFlowResults.h"
#include "AOTFunctionAnalyses.h"
#include "AOTFunctionEditing.h"
#include "AOTIRBuilder.h"
#include "AOTIRPrinter.h"
#include "AOTResultRules.h"
#include "AOTThreads.h"
#include "AOTTypePropagation.h"
#include "AOTValueIdentity.h"
#include "PureNaN.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <optional>
#include <span>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/Hasher.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet constantNodeOpcodes { Opcode::Undefined, Opcode::Null, Opcode::Boolean, Opcode::Integer, Opcode::Double, Opcode::String, Opcode::BigInt };

namespace {

struct ConstructionSite {
    ValueID construction;
    ConstantCollectionKind kind;
    DataID existingData;
    DataNode contents;
    Vector<ValueID> deadValues;
    DataID data;
};

struct FunctionSites {
    FunctionID function;
    Vector<ConstructionSite> sites;
};

using KeyIdentity = std::pair<uint8_t, uint64_t>;
using KeyIdentityTraits = WTF::PairHashTraits<WTF::UnsignedWithZeroKeyHashTraits<uint8_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>;
using PositionsByKey = UncheckedKeyHashMap<KeyIdentity, uint32_t, DefaultHash<KeyIdentity>, KeyIdentityTraits>;
using RecordsByHash = UncheckedKeyHashMap<uint64_t, Vector<DataID, 1>, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>;

} // anonymous namespace

static AnalysisContext contextOf(const AnalysisContext& programContext, FunctionID id)
{
    return { programContext.build, programContext.program, programContext.flow, programContext.effects, id };
}

static Opcode operationFor(ConstantCollectionKind kind)
{
    switch (kind) {
    case ConstantCollectionKind::Set:
        return Opcode::NewSetFromData;
    case ConstantCollectionKind::Map:
        return Opcode::NewMapFromData;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

template<typename Node>
static Node& keyOf(Node& entry, ConstantCollectionKind kind)
{
    switch (kind) {
    case ConstantCollectionKind::Set:
        return entry;
    case ConstantCollectionKind::Map:
        return entry.template as<DataNode::Array>().elements[0];
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isPrimitive(const DataNode& node)
{
    switch (node.kind()) {
    case DataNode::Kind::Undefined:
    case DataNode::Kind::Null:
    case DataNode::Kind::Boolean:
    case DataNode::Kind::Number:
    case DataNode::Kind::String:
    case DataNode::Kind::BigInt:
        return true;
    case DataNode::Kind::Hole:
    case DataNode::Kind::Array:
    case DataNode::Kind::Object:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isHole(const DataNode& node)
{
    return node.is<DataNode::Hole>();
}

static bool isUsableAs(const DataNode::Array& contents, ConstantCollectionKind kind)
{
    if (contents.elements.isEmpty())
        return false;
    return std::ranges::all_of(contents.elements, [&](const DataNode& entry) {
        switch (kind) {
        case ConstantCollectionKind::Set:
            return !isHole(entry);
        case ConstantCollectionKind::Map: {
            const auto* pair = entry.tryAs<DataNode::Array>();
            return pair && pair->elements.size() == 2 && std::ranges::none_of(pair->elements, isHole);
        }
        }
        RELEASE_ASSERT_NOT_REACHED();
    });
}

static bool createsObjects(const DataNode::Array& contents, ConstantCollectionKind kind)
{
    return !std::ranges::all_of(contents.elements, [&](const DataNode& entry) {
        switch (kind) {
        case ConstantCollectionKind::Set:
            return isPrimitive(entry);
        case ConstantCollectionKind::Map:
            return std::ranges::all_of(entry.as<DataNode::Array>().elements, isPrimitive);
        }
        RELEASE_ASSERT_NOT_REACHED();
    });
}

static bool isNegativeZero(const DataNode& node)
{
    const auto* number = node.tryAs<DataNode::Number>();
    return number && !number->value && std::signbit(number->value);
}

static std::optional<KeyIdentity> identityOf(const DataNode& key)
{
    auto identity = [&](uint64_t payload) {
        return KeyIdentity { static_cast<uint8_t>(key.kind()), payload };
    };

    switch (key.kind()) {
    case DataNode::Kind::Undefined:
    case DataNode::Kind::Null:
        return identity(0);
    case DataNode::Kind::Boolean:
        return identity(key.as<DataNode::Boolean>().value);
    case DataNode::Kind::Number: {
        double number = key.as<DataNode::Number>().value;
        return identity(std::bit_cast<uint64_t>(number ? purifyNaN(number) : 0.0));
    }
    case DataNode::Kind::String:
        return identity(key.as<DataNode::String>().value.index());
    case DataNode::Kind::BigInt:
        return identity(key.as<DataNode::BigInt>().digits.index());
    case DataNode::Kind::Hole:
    case DataNode::Kind::Array:
    case DataNode::Kind::Object:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<Vector<DataNode>> removeRepeatedKeys(std::span<const DataNode> entries, ConstantCollectionKind kind)
{
    PositionsByKey positions;
    Vector<uint32_t> sources;
    sources.reserveInitialCapacity(entries.size());
    bool changesEntries = false;
    for (uint32_t index = 0; index < entries.size(); ++index) {
        const DataNode& key = keyOf(entries[index], kind);
        changesEntries |= isNegativeZero(key);
        if (std::optional<KeyIdentity> identity = identityOf(key)) {
            auto position = positions.add(*identity, static_cast<uint32_t>(sources.size()));
            if (!position.isNewEntry) {
                // A repeated key keeps its first position and takes the last value. Equal keys are interchangeable.
                sources[position.iterator->value] = index;
                changesEntries = true;
                continue;
            }
        }
        sources.append(index);
    }
    if (!changesEntries)
        return std::nullopt;

    Vector<DataNode> result;
    result.reserveInitialCapacity(sources.size());
    for (uint32_t source : sources) {
        DataNode entry = entries[source];
        if (isNegativeZero(keyOf(entry, kind)))
            keyOf(entry, kind) = DataNode(DataNode::Number { 0 });
        result.append(WTF::move(entry));
    }
    return result;
}

namespace {

class LiteralReader {
public:
    LiteralReader(const AnalysisContext& context, const Function& function)
        : m_program(context.program)
        , m_function(function)
        , m_analyses(context, function)
    {
    }

    FunctionAnalyses& analyses() { return m_analyses; }

    std::optional<ConstructionSite> tryRead(ValueID construction, ConstantCollectionKind kind)
    {
        ConstructionSite site { .construction = construction, .kind = kind };
        ValueID argument = m_function.at(construction).argumentsOperands()[0];
        // What flow proved about the array stands between it and the constructor as a cast.
        ValueID definition = argument;
        while (isCastWithoutTest(m_function.at(definition).opcode))
            definition = m_function.at(definition).operand(0);
        bool isData = m_function.at(definition).opcode == Opcode::Data;
        if (!(isData ? tryReadData(argument, definition, site) : tryReadLiteral(argument, site)))
            return std::nullopt;

        const auto& contents = (site.existingData ? m_program.at(site.existingData).root : site.contents).as<DataNode::Array>();
        if (std::optional<Vector<DataNode>> entries = removeRepeatedKeys(contents.elements.span(), kind)) {
            site.contents = DataNode(DataNode::Array { contents.contents, WTF::move(*entries) });
            site.existingData = { };
        }
        return site;
    }

private:
    bool tryReadData(ValueID argument, ValueID definition, ConstructionSite& site)
    {
        DataID data = m_function.at(definition).data();
        const auto* contents = m_program.at(data).root.tryAs<DataNode::Array>();
        if (!contents || !isUsableAs(*contents, site.kind))
            return false;

        const UseLists& useLists = m_analyses.useLists();
        ValueID user = site.construction;
        for (ValueID id = argument;; id = m_function.at(id).valueOperand()) {
            std::optional<Use> use = useLists.singleUse(id);
            if (!use || use->kind != Use::Kind::Operand || use->user != user || useLists.locationOf(id).placement != ValuePlacement::InBlock)
                return false;
            site.deadValues.append(id);
            if (id == definition)
                break;
            user = id;
        }

        // Every collection built from one array shares its objects, so the array has to be created exactly once per collection.
        if (createsObjects(*contents, site.kind) && useLists.blockOf(definition) != useLists.blockOf(site.construction))
            return false;

        site.existingData = data;
        return true;
    }

    bool tryReadLiteral(ValueID argument, ConstructionSite& site)
    {
        const EscapeAnalysis& escape = m_analyses.escape();
        std::optional<ObjectID> list = findPrivateLiteral(argument, site.construction);
        if (!list)
            return false;
        ValueID literalID = escape.at(*list).definition;
        const Value& literal = m_function.at(literalID);
        if (literal.elementsOperands().empty())
            return false;

        site.deadValues.append(escape.aliasesOf(*list));
        Vector<DataNode> entries;
        entries.reserveInitialCapacity(literal.elementsOperands().size());
        for (ValueID element : literal.elementsOperands()) {
            std::optional<DataNode> entry = tryReadEntry(element, literalID, site);
            if (!entry)
                return false;
            entries.append(WTF::move(*entry));
        }
        site.contents = DataNode(DataNode::Array { literal.contents(), WTF::move(entries) });
        return true;
    }

    std::optional<DataNode> tryReadEntry(ValueID element, ValueID list, ConstructionSite& site)
    {
        switch (site.kind) {
        case ConstantCollectionKind::Set:
            return constantNodeOf(element);
        case ConstantCollectionKind::Map:
            return tryReadPair(element, list, site.deadValues);
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    std::optional<DataNode> tryReadPair(ValueID element, ValueID list, Vector<ValueID>& deadValues)
    {
        const EscapeAnalysis& escape = m_analyses.escape();
        std::optional<ObjectID> pair = findPrivateLiteral(element, list);
        if (!pair)
            return std::nullopt;
        const Value& literal = m_function.at(escape.at(*pair).definition);
        std::span<const ValueID> members = literal.elementsOperands();
        if (members.size() != 2)
            return std::nullopt;

        std::optional<DataNode> key = constantNodeOf(members[0]);
        std::optional<DataNode> value = constantNodeOf(members[1]);
        if (!key || !value)
            return std::nullopt;
        deadValues.append(escape.aliasesOf(*pair));
        return DataNode(DataNode::Array { literal.contents(), Vector<DataNode>::from(WTF::move(*key), WTF::move(*value)) });
    }

    std::optional<ObjectID> findPrivateLiteral(ValueID value, ValueID user)
    {
        const EscapeAnalysis& escape = m_analyses.escape();
        std::optional<ObjectID> object = escape.singleObject(value);
        if (!object)
            return std::nullopt;
        const Value& literal = m_function.at(escape.at(*object).definition);
        if (literal.opcode != Opcode::NewArray || !literal.holes().empty() || literal.capacityOperand())
            return std::nullopt;
        std::span<const ObjectUse> uses = escape.usesOf(*object);
        if (uses.size() != 1 || uses.front().use.kind != Use::Kind::Operand || uses.front().use.user != user)
            return std::nullopt;
        if (!std::ranges::all_of(escape.aliasesOf(*object), [&](ValueID alias) { return canEraseWhenUnused(alias); }))
            return std::nullopt;
        return object;
    }

    bool canEraseWhenUnused(ValueID id)
    {
        return m_analyses.useLists().locationOf(id).placement == ValuePlacement::InBlock
            && m_analyses.effectsOf(id).onAnyEdge().isRemovableWhenUnused();
    }

    std::optional<DataNode> constantNodeOf(ValueID id) const
    {
        std::optional<ValueID> constantID = constantThroughCasts(m_program, m_function, id);
        if (!constantID)
            return std::nullopt;
        const Value& constant = m_function.at(*constantID);
        if (!constantNodeOpcodes.contains(constant.opcode))
            return std::nullopt;

        switch (constant.opcode) {
        case Opcode::Undefined:
            return DataNode(DataNode::Undefined { });
        case Opcode::Null:
            return DataNode(DataNode::Null { });
        case Opcode::Boolean:
            return DataNode(DataNode::Boolean { constant.boolean() });
        case Opcode::Integer:
            return DataNode(DataNode::Number { static_cast<double>(constant.integer()) });
        case Opcode::Double:
            return DataNode(DataNode::Number { constant.number() });
        case Opcode::String:
            return DataNode(DataNode::String { constant.string() });
        case Opcode::BigInt:
            return DataNode(DataNode::BigInt { constant.digits() });
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }

    const Program& m_program;
    const Function& m_function;
    FunctionAnalyses m_analyses;
};

} // anonymous namespace

static std::optional<ConstantCollectionKind> collectionKindOf(FunctionAnalyses& analyses, ValueID id)
{
    const Value& call = analyses.function().at(id);
    if (call.opcode != Opcode::CallBuiltin || call.builtinCallForm() != BuiltinCallForm::Whole || call.receiverOperand() || call.argumentsOperands().size() != 1)
        return std::nullopt;

    const BuiltinTable& builtins = analyses.program().builtins();
    const BuiltinOverload& overload = builtins.at(call.overload());
    if (overload.mode != BuiltinCallMode::Construct)
        return std::nullopt;

    // A private literal of constants is iterated, and its entries are added, through locked prototypes.
    BuiltinID constructor = builtins.canonicalID(overload.builtin);
    if (constructor == builtins.at(BuiltinClasses::Set).constructor)
        return ConstantCollectionKind::Set;
    if (constructor == builtins.at(BuiltinClasses::Map).constructor)
        return ConstantCollectionKind::Map;
    return std::nullopt;
}

static void collectSites(const AnalysisContext& context, Vector<FunctionSites>& found)
{
    const Function& function = context.program.at(context.id);
    if (!function.hasBody() || !context.build.isEnabled(PassID::EmbedConstantCollections, context.id))
        return;

    std::optional<LiteralReader> reader;
    Vector<ConstructionSite> sites;
    for (const Block& block : function.blocks) {
        for (ValueID id : block.values) {
            if (function.at(id).opcode != Opcode::CallBuiltin)
                continue;

            if (!reader)
                reader.emplace(context, function);
            std::optional<ConstantCollectionKind> kind = collectionKindOf(reader->analyses(), id);
            if (!kind)
                continue;
            if (std::optional<ConstructionSite> site = reader->tryRead(id, *kind))
                sites.append(WTF::move(*site));
        }
    }
    if (!sites.isEmpty())
        found.append(FunctionSites { context.id, WTF::move(sites) });
}

static void add(Hasher& hasher, const DataNode& node)
{
    add(hasher, node.kind());
    switch (node.kind()) {
    case DataNode::Kind::Undefined:
    case DataNode::Kind::Null:
    case DataNode::Kind::Hole:
        return;
    case DataNode::Kind::Boolean:
        add(hasher, node.as<DataNode::Boolean>().value);
        return;
    case DataNode::Kind::Number:
        add(hasher, node.as<DataNode::Number>().value);
        return;
    case DataNode::Kind::String:
        add(hasher, node.as<DataNode::String>().value);
        return;
    case DataNode::Kind::BigInt:
        add(hasher, node.as<DataNode::BigInt>().digits);
        return;
    case DataNode::Kind::Array: {
        auto& array = node.as<DataNode::Array>();
        add(hasher, array.contents);
        add(hasher, static_cast<uint32_t>(array.elements.size()));
        for (const DataNode& element : array.elements)
            add(hasher, element);
        return;
    }
    case DataNode::Kind::Object: {
        auto& object = node.as<DataNode::Object>();
        add(hasher, object.layout);
        add(hasher, static_cast<uint32_t>(object.members.size()));
        for (const DataMember& member : object.members) {
            add(hasher, member.key);
            add(hasher, member.value);
        }
        return;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static DataID findOrAddData(ProgramEditor& editor, RecordsByHash& recordsByHash, Data&& data)
{
    Vector<DataID, 1>& records = recordsByHash.add(computeHash(data.root), Vector<DataID, 1>()).iterator->value;
    for (DataID record : records) {
        if (editor.program().at(record).root == data.root)
            return record;
    }
    DataID id = editor.addData(WTF::move(data));
    records.append(id);
    return id;
}

static void eraseDeadValues(IRBuilder& builder, const BitVector& deadValues)
{
    Function& function = builder.function();
    for (unsigned blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        BlockID block(blockIndex);
        const BlockValues& values = function.at(block).values;
        BitVector deadIndexes;
        for (size_t index = 0; index < values.size(); ++index) {
            if (deadValues.quickGet(values[index].index()))
                deadIndexes.set(index);
        }
        if (!deadIndexes.isEmpty())
            builder.eraseValues(block, deadIndexes);
    }
}

static void rewriteFunction(const AnalysisContext& context, Function& function, TypeArena& types, std::span<const ConstructionSite> sites)
{
    const Program& program = context.program;
    BitVector deadValues(function.values.size());
    Vector<ValueID> constructions;
    constructions.reserveInitialCapacity(sites.size());
    for (const ConstructionSite& site : sites) {
        Value& construction = function.at(site.construction);
        AllocationSiteID allocationSite = construction.allocationSite();
        construction.become(operationFor(site.kind));
        construction.setData(site.data);
        construction.setAllocationSite(allocationSite);
        construction.type = resultTypeOf(program, function, construction, types);
        construction.setWritten({ });
        construction.setPrediction({ });
        constructions.append(site.construction);
        for (ValueID dead : site.deadValues)
            deadValues.quickSet(dead.index());
    }

    FunctionAnalyses analyses(context, function);
    IRBuilder builder(context, function, types, &analyses);
    eraseDeadValues(builder, deadValues);
    narrowValueTypes(TypePropagationInputs { program, context.id, function, analyses, types, context.build }, constructions.span());
    compactFunction(function);
}

ConstantCollections embedConstantCollections(ProgramEditor& editor, const FlowResults& flow, BuildContext& build)
{
    const Program& program = editor.program();
    EffectSummaries summaries = createSummariesFromFlow(program, flow, build);
    AnalysisContext programContext { build, program, &flow, &summaries };
    Vector<FunctionSites> found = collectInParallel<FunctionSites>(build.parallelLoops(), program.functions.size(), [&](unsigned index, Vector<FunctionSites>& foundInChunk) {
        collectSites(contextOf(programContext, FunctionID(index)), foundInChunk);
    });

    ConstantCollections collections;
    RecordsByHash recordsByHash;
    uint64_t numberOfSites = 0;
    for (FunctionSites& entry : found) {
        ModuleID module = program.at(entry.function).module;
        for (ConstructionSite& site : entry.sites) {
            site.data = site.existingData ? site.existingData : findOrAddData(editor, recordsByHash, Data { WTF::move(site.contents), module });
            collections.m_collections.append(ConstantCollection { site.data, site.kind });
        }
        numberOfSites += entry.sites.size();
    }

    Vector<FunctionID> functions(found.size(), [&](size_t index) {
        return found[index].function;
    });
    editFunctionsInParallel(editor, build, functions.span(), [&](unsigned index, FunctionID id, Function& function, TaskArenas& arenas) {
        rewriteFunction(contextOf(programContext, id), function, arenas.types, found[index].sites.span());
    });

    sortAndRemoveDuplicates(build.parallelLoops(), collections.m_collections);
    build.statistics().count(StatisticID::ConstantCollectionsEmbedded, numberOfSites);
    build.destroyLater(WTF::move(summaries));
    return collections;
}

static void dumpNode(IRPrinter& printer, const DataNode& node)
{
    auto dumpLeaf = [&](auto... payload) {
        printer.out().print(nameOf(node.kind()), "(");
        (printer.print(payload), ...);
        printer.out().print(")");
    };

    switch (node.kind()) {
    case DataNode::Kind::Undefined:
    case DataNode::Kind::Null:
    case DataNode::Kind::Hole:
        dumpLeaf();
        return;
    case DataNode::Kind::Boolean:
        dumpLeaf(node.as<DataNode::Boolean>().value);
        return;
    case DataNode::Kind::Number:
        dumpLeaf(node.as<DataNode::Number>().value);
        return;
    case DataNode::Kind::String:
        dumpLeaf(node.as<DataNode::String>().value);
        return;
    case DataNode::Kind::BigInt:
        dumpLeaf(node.as<DataNode::BigInt>().digits);
        return;
    case DataNode::Kind::Array:
    case DataNode::Kind::Object:
        printVariant(printer, node);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void ConstantCollections::dump(PrintStream& out, const Program& program) const
{
    IRPrinter printer(out, program);
    for (size_t index = 0; index < m_collections.size(); ++index) {
        const ConstantCollection& collection = m_collections[index];
        std::span<const DataNode> entries = program.at(collection.data).root.as<DataNode::Array>().elements.span();
        out.print("table ", index, " keys: ");
        dumpList(out, entries, [&](PrintStream&, const DataNode& entry) {
            dumpNode(printer, keyOf(entry, collection.kind));
        });
        out.print("\n");
        if (collection.kind == ConstantCollectionKind::Set)
            continue;

        out.print("table ", index, " values: ");
        dumpList(out, entries, [&](PrintStream&, const DataNode& entry) {
            dumpNode(printer, entry.as<DataNode::Array>().elements[1]);
        });
        out.print("\n");
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
