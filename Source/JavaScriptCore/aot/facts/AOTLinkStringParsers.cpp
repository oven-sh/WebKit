/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// What the scan and Link know of parsing by a written type.
// STEP ONE, WHICH IS ALL THAT STANDS HERE: a parser makes objects where no allocation of the program stands, so each layout that a parser of a reached call may make is allocated and escapes.
// linked() then proves nothing of it: every field holds anything, every change is possible. Facts::parsersMayMake() lists such layouts alone, and no parser, compiled or of the run time, makes an object of another.
// Both readings run this. In held() what escapes is only read and called, so its fields keep what the program's own stores give them, and a guess by shape passes on what was parsed.

#include "config.h"
#include "AOTLinkStringParsers.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuiltinTable.h"
#include "AOTIRProgram.h"
#include "AOTSort.h"
#include "AOTTypeArena.h"
#include "AOTWalkStringParsers.h"
#include <algorithm>
#include <mutex>
#include <wtf/HashSet.h>
#include <wtf/NeverDestroyed.h>

namespace JSC { namespace AOT { namespace Typed {

TypeID typeParsedAt(const ThinSummary& summary, ValueID site)
{
    auto found = std::ranges::lower_bound(summary.typesParsedAtCalls, site, { }, &SummaryTypeParsed::site);
    return found != summary.typesParsedAtCalls.end() && found->site == site ? found->type : TypeID();
}

// ---- Which calls are followed

namespace {

struct ParsersByTypeInTable {
    // By canonical built-in.
    BitVector functions;
    // Under which a program finds one of them.
    Vector<BuiltinName, 8> names;
    // Of those, the names under which every parser is a member of ONE object (`JSON.parse`), none of a prototype object (`response.json`). By canonical built-in: those objects.
    Vector<BuiltinName, 8> namesFoundOnOneObjectAlone;
    BitVector holders;
};

} // anonymous namespace

// The table of built-ins is the same for every build of a process.
static const ParsersByTypeInTable& parsersByTypeIn(const BuiltinTable& builtins)
{
    static LazyNeverDestroyed<ParsersByTypeInTable> parsers;
    static std::once_flag once;
    std::call_once(once, [&] {
        parsers.construct();
        for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
            BuiltinID canonical = builtins.canonicalID(BuiltinID(index));
            bool parsesByType = false;
            for (const BuiltinOverload& overload : builtins.overloadsOf(canonical))
                parsesByType |= overload.mode == BuiltinCallMode::Call && overload.hasTypedResult();
            if (!parsesByType)
                continue;
            parsers->functions.set(canonical.index());
            BuiltinName name = builtins.at(BuiltinID(index)).name;
            if (!!name && !parsers->names.contains(name))
                parsers->names.append(name);
        }
        for (BuiltinName name : parsers->names) {
            bool isFoundOnOneObjectAlone = true;
            for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
                const Builtin& entry = builtins.at(BuiltinID(index));
                if (entry.name != name || !parsers->functions.get(builtins.canonicalID(BuiltinID(index)).index()))
                    continue;
                if (!entry.holder || builtins.at(entry.holder).kind == BuiltinKind::Prototype)
                    isFoundOnOneObjectAlone = false;
                else
                    parsers->holders.set(builtins.canonicalID(entry.holder).index());
            }
            if (isFoundOnOneObjectAlone)
                parsers->namesFoundOnOneObjectAlone.append(name);
        }
    });
    return parsers;
}

void ResultsOfParsersInScan::listKeysOfParsers(const Program& program)
{
    m_hasListedKeysOfParsers = true;
    const ParsersByTypeInTable& parsers = parsersByTypeIn(program.builtins());
    for (BuiltinName name : parsers.names) {
        PropertyKeyID keyOfParser = program.keyOf(name);
        if (!keyOfParser)
            continue;
        m_keysOfParsers.append(keyOfParser);
        if (parsers.namesFoundOnOneObjectAlone.contains(name))
            m_keysFoundOnOneObjectAlone.append(keyOfParser);
    }
}

ValueID ResultsOfParsersInScan::operandWhoseMakerIsAsked(const Program&, const BodyOperation<Opcode::Call>& call)
{
    if (call.calledBuiltin())
        return { };
    PropertyKeyID key = call.calleeKey();
    if (!key)
        return call.calleeOperand();
    return m_keysFoundOnOneObjectAlone.contains(key) ? call.thisValueOperand() : ValueID();
}

// Asked of every Call of every body. Nearly all of them are calls under a name, and the names of the parsers are a handful of keys of the program: so the key is compared with those, and nothing is looked up.
bool ResultsOfParsersInScan::mayCallParserByType(const Program& program, const BodyOperation<Opcode::Call>& call, const BodyValue& definitionOfCallee)
{
    PropertyKeyID key = call.calleeKey();
    BuiltinID called = call.calledBuiltin();
    if (key && !called) {
        if (!m_keysOfParsers.contains(key))
            return false;
        if (!m_keysFoundOnOneObjectAlone.contains(key))
            return true;
        // `schema.parse(value)`, `path.parse(name)`: the name alone says little, and each type that is asked of such a call would get a parser. `JSON.parse(text)` names the object.
        const BuiltinTable& builtins = program.builtins();
        return definitionOfCallee && definitionOfCallee.opcode() == Opcode::Builtin && parsersByTypeIn(builtins).holders.get(builtins.canonicalID(definitionOfCallee.as<Opcode::Builtin>().builtin()).index());
    }

    const BuiltinTable& builtins = program.builtins();
    const ParsersByTypeInTable& parsers = parsersByTypeIn(builtins);
    auto parsesByType = [&](BuiltinID builtin) {
        return builtin && parsers.functions.get(builtins.canonicalID(builtin).index());
    };
    if (parsesByType(called))
        return true;
    if (key) {
        BuiltinName name = program.builtinNameOf(key);
        return !!name && parsers.names.contains(name);
    }
    return definitionOfCallee && definitionOfCallee.opcode() == Opcode::Builtin && parsesByType(definitionOfCallee.as<Opcode::Builtin>().builtin());
}

// ---- What the operations behind a call ask of its result

void ResultsOfParsersInScan::reset()
{
    m_values.shrink(0);
    m_calls.shrink(0);
}

auto ResultsOfParsersInScan::find(ValueID value) const -> const FollowedValue*
{
    auto found = std::ranges::lower_bound(m_values, value, { }, &FollowedValue::value);
    return found != m_values.end() && found->value == value ? &*found : nullptr;
}

// A type without a layout and without an array gives a parser nothing to make.
void ResultsOfParsersInScan::request(const Program& program, uint32_t call, TypeID requested)
{
    if (!requested || m_calls[call].requestedTypes.contains(requested))
        return;
    Type type = program.at(requested);
    if (type.layouts || findClassPart(program, type, BuiltinClasses::Array))
        m_calls[call].requestedTypes.append(requested);
}

static TypeID typeThatCastAsksOfResultOfParser(const BodyValue& cast)
{
    switch (cast.opcode()) {
    case Opcode::Convert:
        return cast.as<Opcode::Convert>().declared().written();
    case Opcode::Check:
        return cast.as<Opcode::Check>().expectedType().predicted();
    case Opcode::Upcast:
        return cast.as<Opcode::Upcast>().castType();
    default:
        return { };
    }
}

void ResultsOfParsersInScan::readOperation(const Program& program, const BodyValue& operation, const BodyValue& definitionOfCallee)
{
    Opcode opcode = operation.opcode();
    if (inputOperationsThatHandOn.contains(opcode)) {
        const FollowedValue* followed = find(operation.operand(0));
        if (!followed)
            return;
        FollowedValue handedOn { operation.id(), followed->call, followed->mayBePromise };
        request(program, handedOn.call, typeThatCastAsksOfResultOfParser(operation));
        m_values.append(handedOn);
        return;
    }

    switch (opcode) {
    case Opcode::Call: {
        auto call = operation.as<Opcode::Call>();
        ExpectedResultID expected = call.expectedResult();
        // The host makes the rows of a query, whoever is called: the record says all.
        bool isRowsOfQuery = expected && program.at(expected).flags.contains(ExpectedResultFlags::IsRowsOfQuery);
        if (!isRowsOfQuery && !mayCallParserByType(program, call, definitionOfCallee))
            return;
        if (!expected)
            m_values.append(FollowedValue { operation.id(), static_cast<uint32_t>(m_calls.size()), true });
        m_calls.append(FollowedCall { .call = operation.id(), .expected = expected });
        return;
    }
    case Opcode::Await: {
        const FollowedValue* followed = find(operation.as<Opcode::Await>().valueOperand());
        if (!followed || !followed->mayBePromise)
            return;
        uint32_t call = followed->call;
        m_calls[call].isAwaited = true;
        m_values.append(FollowedValue { operation.id(), call, false });
        return;
    }
    case Opcode::PropGet: {
        auto read = operation.as<Opcode::PropGet>();
        if (const FollowedValue* followed = find(read.objectOperand())) {
            FollowedCall& call = m_calls[followed->call];
            (followed->mayBePromise ? call.namesReadInFrontOfAwait : call.namesRead).append(read.key());
        }
        return;
    }
    case Opcode::TypeTest: {
        auto test = operation.as<Opcode::TypeTest>();
        if (const FollowedValue* followed = find(test.valueOperand()))
            request(program, followed->call, test.testedType());
        return;
    }
    case Opcode::CallDirect: {
        auto call = operation.as<Opcode::CallDirect>();
        const Function& callee = program.at(call.function());
        auto arguments = call.argumentsOperands();
        for (uint32_t parameter = 0; parameter < arguments.size() && parameter < callee.numberOfEntryParameters(); ++parameter) {
            if (callee.entryParameterKind(parameter) != EntryParameter::Parameter)
                continue;
            const FollowedValue* followed = find(arguments[parameter]);
            const DeclaredType* declared = followed ? callee.declaredEntryParameter(parameter) : nullptr;
            if (declared)
                request(program, followed->call, declared->written());
        }
        return;
    }
    default:
        return;
    }
}

void ResultsOfParsersInScan::readReturnCall(const Program& program, const BodyTerminator& terminator)
{
    if (m_calls.isEmpty())
        return;
    auto returned = terminator.as<TerminatorKind::ReturnCall>();
    const FollowedValue* followed = find(returned.callOperand());
    if (!followed)
        return;
    uint32_t call = followed->call;
    for (TailCast cast : returned.casts())
        request(program, call, cast.declared.written());
}

static bool isModuleOfLibraryForTypedResults(const Program& program, ModuleID module)
{
    return module && (program.at(module).kind == ModuleKind::RuntimeSource || program.at(module).flags.contains(ModuleFlags::IsLibrary));
}

// In a file that nobody has checked a written type may be a guess of the front end. One that declares none of the names that are read is a wrong one.
static bool declaresNamesReadOffTypedResult(const Program& program, const Function& function, TypeID requested, std::span<const PropertyKeyID> namesRead)
{
    bool hasRecords = false;
    bool isAskedByLibrary = isModuleOfLibraryForTypedResults(program, function.module);
    for (LayoutID member : membersOf(program, program.at(requested))) {
        const Layout& layout = program.at(member);
        if (layout.kind != LayoutKind::Record)
            continue;
        hasRecords = true;
        if (!isAskedByLibrary && isModuleOfLibraryForTypedResults(program, layout.module))
            continue;
        if (std::ranges::all_of(namesRead, [&](PropertyKeyID name) { return !!layout.findField(name); }))
            return true;
    }
    return !hasRecords;
}

void ResultsOfParsersInScan::finish(const Program& program, const Function& function, ThinSummary& summary)
{
    for (const FollowedCall& call : m_calls) {
        if (call.expected) {
            summary.typesParsedAtCalls.append({ call.call, program.at(call.expected).type });
            continue;
        }
        if (!function.has(FunctionFlags::IsChecked)) {
            Vector<PropertyKeyID, 8> namesRead;
            namesRead.append(call.namesRead.span());
            if (!call.isAwaited)
                namesRead.append(call.namesReadInFrontOfAwait.span());
            // Which of several types stays is settled behind the scan, so each of them has to declare the names.
            bool everyTypeDeclaresNames = std::ranges::all_of(call.requestedTypes, [&](TypeID requested) {
                return declaresNamesReadOffTypedResult(program, function, requested, namesRead.span());
            });
            if (!everyTypeDeclaresNames)
                continue;
        }
        for (TypeID requested : call.requestedTypes)
            summary.typesParsedAtCalls.append({ call.call, requested });
    }
}

// ---- The one type that is asked for

// isSubset() does not look at what is written of the contents of a class.
static bool isSubsetWithContentsForTypedResult(TypeArena& types, TypeID narrow, TypeID wide)
{
    struct Pair {
        TypeID narrow;
        TypeID wide;
    };
    Vector<Pair, 4> worklist { Pair { narrow, wide } };
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> visited;
    while (!worklist.isEmpty()) {
        Pair pair = worklist.takeLast();
        if (pair.narrow == pair.wide || !visited.add(static_cast<uint64_t>(pair.narrow.bits()) << 32 | pair.wide.bits()).isNewEntry)
            continue;
        if (!types.isSubset(pair.narrow, pair.wide))
            return false;
        Type wideType = types.at(pair.wide);
        for (const ClassPart& part : classPartsOf(types, types.at(pair.narrow))) {
            const ClassPart* partner = findClassPart(types, wideType, part.builtinClass);
            if (!partner)
                continue;
            for (unsigned index = 0; index < mostContentsOfClass; ++index) {
                if (partner->writtenContents[index])
                    worklist.append(Pair { part.writtenContents[index] ? part.writtenContents[index] : TypeTable::anything, partner->writtenContents[index] });
            }
        }
    }
    return true;
}

// The most specific of the types, where every other one takes all that it takes. Null: two users ask for types of which neither holds the other.
static TypeID mostSpecificTypeAskedOfResultOfParser(TypeArena& types, std::span<const SummaryTypeParsed> requests)
{
    TypeID mostSpecific;
    for (const SummaryTypeParsed& request : requests) {
        if (!mostSpecific || isSubsetWithContentsForTypedResult(types, request.type, mostSpecific))
            mostSpecific = request.type;
    }
    for (const SummaryTypeParsed& request : requests) {
        if (!isSubsetWithContentsForTypedResult(types, mostSpecific, request.type))
            return { };
    }
    return mostSpecific;
}

static void settleTypesParsedAtCalls(TypeArena& types, Vector<SummaryTypeParsed>& rows)
{
    size_t kept = 0;
    for (size_t begin = 0; begin < rows.size();) {
        size_t end = begin + 1;
        while (end < rows.size() && rows[end].site == rows[begin].site)
            ++end;
        TypeID type = end - begin == 1 ? rows[begin].type : mostSpecificTypeAskedOfResultOfParser(types, rows.span().subspan(begin, end - begin));
        if (type)
            rows[kept++] = { rows[begin].site, type };
        begin = end;
    }
    rows.shrink(kept);
}

void appendParsersOfTypesThatScanHasFound(Program& program, BodyIndexTable& bodies, ThinSummaries& summaries, Diagnostics& diagnostics, Mechanisms mechanisms)
{
    if (!parsersAreWrittenUnder(mechanisms))
        return;

    Vector<TypeID> roots;
    {
        TypeArena types(program.types, ArenaKeeps::InProgram);
        for (uint32_t index = 0; index < summaries.size(); ++index) {
            Vector<SummaryTypeParsed>& rows = summaries.at(FunctionID(index)).typesParsedAtCalls;
            if (rows.isEmpty())
                continue;
            settleTypesParsedAtCalls(types, rows);
            for (const SummaryTypeParsed& row : rows)
                roots.append(row.type);
        }
    }
    program.appendParsersFor(roots.span());

    auto numberOfFunctions = static_cast<uint32_t>(program.functions.size());
    bodies.addFunctionsWithoutBodies(numberOfFunctions);
    scanBodiesAppendedToProgram(program, diagnostics, summaries, mechanisms);

    // An allocation creates only a row of fields whose order the image holds. The order in which a layout declares its fields is there anyway, and every beginning of it.
    for (uint32_t index = numberOfFunctions; index-- && program.functions[index].role == FunctionRole::Parser;) {
        Vector<uint32_t>& lists = summaries.at(FunctionID(index)).fieldsListedByAllocations;
        forEachLayoutParsedAt(program, program.functions[index].thisType.expected().predicted(), scopedLambda<void(LayoutID)>([&](LayoutID layout) {
            if (!isRecordThatParsersCanMake(program, layout))
                return;
            Vector<uint32_t, 8> fields = fieldsThatTextMayNotLeaveOut(program, layout);
            std::span<const uint32_t> declared = program.at(layout).declaredOrder.span();
            if (fields.isEmpty() || (fields.size() <= declared.size() && std::ranges::equal(fields, declared.first(fields.size()))))
                return;
            lists.append(layout.index());
            lists.append(static_cast<uint32_t>(fields.size()));
            lists.appendVector(fields);
        }));
    }
}

// ---- Link

void stateObjectsMadeByParsersInLink(LinkFlow& flow, FunctionID, ValueID, TypeID parsed, FlowNodeID result)
{
    if (result)
        flow.arrive(result, LinkType::anything());
    if (!parsed)
        return;

    // What has no body is reached from the start (StateOfLinkReach::enterProgram()), so that says nothing of a parser. A parser that a row has led to has had its say, with all that lies below it: each is looked at once in a flow, whatever the number of calls.
    const Program& program = flow.program;
    Vector<TypeID, 8> worklist { parsed };
    while (!worklist.isEmpty()) {
        TypeID type = worklist.takeLast();
        FunctionID parser = parserFunctionOf(program, type);
        if (!parser || flow.libraryFunctionsEntered.set(parser.index()))
            continue;
        flow.reach(parser);
        forEachLayoutParsedAt(program, type, scopedLambda<void(LayoutID)>([&](LayoutID layout) {
            if (!isRecordThatParsersCanMake(program, layout))
                return;
            flow.noteAllocated(layout);
            flow.escape(layout);
        }));
        forEachTypeParsedBelow(program, type, scopedLambda<void(TypeID)>([&](TypeID below) {
            worklist.append(below);
        }));
    }
}

void forEachParserNamedBy(const Program& program, const ThinSummary& summary, FunctionID referrer, NOESCAPE const ScopedLambda<void(FunctionID)>& name)
{
    for (const SummaryTypeParsed& parsed : summary.typesParsedAtCalls) {
        if (FunctionID parser = parserFunctionOf(program, parsed.type))
            name(parser);
    }
    const Function& function = program.at(referrer);
    if (function.role != FunctionRole::Parser)
        return;
    forEachTypeParsedBelow(program, function.thisType.expected().predicted(), scopedLambda<void(TypeID)>([&](TypeID below) {
        if (FunctionID parser = parserFunctionOf(program, below))
            name(parser);
    }));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
