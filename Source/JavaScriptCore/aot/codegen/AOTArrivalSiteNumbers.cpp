/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTArrivalSiteNumbers.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTDiagnostics.h"
#include "AOTFlowResults.h"
#include "AOTIR.h"
#include "AOTPropertyAccessReceivers.h"
#include "AOTSchemaWalkers.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <tuple>
#include <utility>
#include <wtf/CommaPrinter.h>
#include <wtf/HashMap.h>
#include <wtf/PrintStream.h>
#include <wtf/text/CString.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT { namespace Typed {

using Wire::ArrivalStamp;
using Wire::TextRange;

namespace {

// An operation that does the work of an access without a name: a slot operation, an element access, a direct call.
struct FastArm {
    AccessSiteID site;
    uint32_t inlineFrame { 0 };
    LayoutSetID receivers;
};

struct ArrivalSiteDescription {
    Opcode operation { Opcode::Undefined };
    Origin origin;
    AccessSiteID accessSite;
    AccessKind kind { AccessKind::Unknown };
    KnowledgeLevel level { KnowledgeLevel::Unknown };
    ArrivalStamp stamp { ArrivalStamp::None };
    LayoutList expected;
};

// Each name is in the text once.
class ReportText {
public:
    ReportText(Vector<uint8_t>& text, const StringTable& strings)
        : m_text(text)
        , m_strings(strings)
    {
    }

    TextRange ensure(Opcode opcode) { return ensureName(m_operations[static_cast<size_t>(opcode)], nameOf(opcode)); }
    TextRange ensure(LayoutKind kind) { return ensureName(m_layoutKinds[static_cast<size_t>(kind)], nameOf(kind)); }

    TextRange ensure(StringID string)
    {
        if (!string || m_strings[string].empty())
            return { };
        return m_rangesOfStrings.ensure(string, [&] {
            if (m_strings.isAllASCII(string))
                return append(m_strings[string]);
            return append(byteCast<uint8_t>(m_strings.toWTFString(string).utf8(StrictConversionReplacingUnpairedSurrogatesWithFFFD).span()));
        }).iterator->value;
    }

private:
    TextRange ensureName(std::optional<TextRange>& range, ASCIILiteral name)
    {
        if (!range)
            range = append(byteCast<uint8_t>(name.span8()));
        return *range;
    }

    TextRange append(std::span<const uint8_t> utf8)
    {
        uint32_t offset = static_cast<uint32_t>(m_text.size());
        m_text.append(utf8);
        return { offset, static_cast<uint32_t>(utf8.size()) };
    }

    Vector<uint8_t>& m_text;
    const StringTable& m_strings;
    std::array<std::optional<TextRange>, numberOfEnumerators<Opcode>> m_operations;
    std::array<std::optional<TextRange>, numberOfEnumerators<LayoutKind>> m_layoutKinds;
    UncheckedKeyHashMap<StringID, TextRange> m_rangesOfStrings;
};

} // anonymous namespace

uint32_t numberOfArrivalSitesOf(const Value& value)
{
    switch (value.opcode) {
    case Opcode::PropGet:
    case Opcode::PropSet:
        // A free name has no receiver.
        return !value.isFreeName();
    case Opcode::PropDefine:
    case Opcode::PropHas:
    case Opcode::ElemGet:
    case Opcode::ElemSet:
    case Opcode::ElemHas:
    case Opcode::ElemDefine:
    case Opcode::PropGetAndTest:
    case Opcode::CallMethod:
        return 1;
    // A call by value. What arrives is the callee.
    case Opcode::Call:
    case Opcode::CallList:
    case Opcode::Construct:
    case Opcode::ConstructList:
        return 1;
    // The stub of a chain keeps the result of the first read to itself, so nothing can record the receiver of the second.
    case Opcode::PropGetChain:
        return 1;
    case Opcode::PropGetPair:
        return 2;
    default:
        return 0;
    }
}

Vector<ArrivalSite> findArrivalSites(const Function& function)
{
    Vector<ArrivalSite> sites;
    for (unsigned index = 0; index < function.blocks.size(); ++index) {
        for (ValueID value : function.blocks[index].values) {
            uint32_t count = numberOfArrivalSitesOf(function.at(value));
            for (uint32_t part = 0; part < count; ++part)
                sites.append(ArrivalSite { .block = BlockID(index), .value = value, .part = part });
        }
    }
    return sites;
}

ArrivalSiteNumbers numberArrivalSites(const Program& program, BuildContext& build)
{
    if (!build.recordedOptions().countsArrivals)
        return { };

    unsigned count = program.functions.size();
    ArrivalSiteNumbers numbers;
    numbers.m_isCounting = true;
    numbers.m_functions = Vector<ArrivalSiteNumbers::SitesOfFunction>(count);
    forEachIndexInParallel(build.parallelLoops(), count, [&](unsigned index) {
        ArrivalSiteNumbers::SitesOfFunction& sites = numbers.m_functions[index];
        sites.inOrder = findArrivalSites(program.functions[index]);
        for (uint32_t ordinal = 0; ordinal < sites.inOrder.size(); ++ordinal) {
            if (!sites.inOrder[ordinal].part)
                sites.firstOrdinalsByValue.append(ordinal);
        }
        AOT::sort(sites.firstOrdinalsByValue, { }, [&](uint32_t ordinal) {
            return sites.inOrder[ordinal].value;
        });
    });

    uint64_t total = 0;
    for (ArrivalSiteNumbers::SitesOfFunction& sites : numbers.m_functions) {
        sites.first = static_cast<uint32_t>(total);
        total += sites.inOrder.size();
        RELEASE_ASSERT(total <= ArrivalSiteNumbers::maxNumberOfSites);
    }
    return numbers;
}

uint32_t ArrivalSiteNumbers::firstOf(FunctionID function) const
{
    return function.index() < m_functions.size() ? m_functions[function.index()].first : 0;
}

uint32_t ArrivalSiteNumbers::numberFor(FunctionID function, uint32_t ordinal) const
{
    RELEASE_ASSERT(ordinal < sitesOf(function).size());
    return firstOf(function) + ordinal;
}

std::optional<uint32_t> ArrivalSiteNumbers::numberOf(FunctionID function, ValueID value, uint32_t part) const
{
    if (function.index() >= m_functions.size())
        return std::nullopt;
    const SitesOfFunction& sites = m_functions[function.index()];
    auto valueAt = [&](uint32_t ordinal) {
        return sites.inOrder[ordinal].value;
    };
    auto first = std::ranges::lower_bound(sites.firstOrdinalsByValue, value, { }, valueAt);
    if (first == sites.firstOrdinalsByValue.end() || valueAt(*first) != value)
        return std::nullopt;
    uint64_t ordinal = static_cast<uint64_t>(*first) + part;
    if (ordinal >= sites.inOrder.size() || valueAt(static_cast<uint32_t>(ordinal)) != value)
        return std::nullopt;
    return sites.first + static_cast<uint32_t>(ordinal);
}

std::span<const ArrivalSite> ArrivalSiteNumbers::sitesOf(FunctionID function) const
{
    if (function.index() >= m_functions.size())
        return { };
    return m_functions[function.index()].inOrder.span();
}

static bool opcodeCallsByValue(Opcode opcode)
{
    return opcode == Opcode::Call || opcode == Opcode::CallList || opcode == Opcode::Construct || opcode == Opcode::ConstructList;
}

static AccessKind accessKindOf(Opcode opcode)
{
    switch (opcode) {
    case Opcode::PropGet:
    case Opcode::ElemGet:
    case Opcode::PropGetChain:
    case Opcode::PropGetPair:
    case Opcode::PropGetAndTest:
    // What goes by name in a call of a method is the read of the method.
    case Opcode::CallMethod:
        return AccessKind::Read;
    case Opcode::PropSet:
    case Opcode::ElemSet:
        return AccessKind::Store;
    case Opcode::PropDefine:
    case Opcode::ElemDefine:
        return AccessKind::Define;
    case Opcode::PropHas:
    case Opcode::ElemHas:
        return AccessKind::Has;
    case Opcode::Call:
    case Opcode::CallList:
        return AccessKind::Call;
    case Opcode::Construct:
    case Opcode::ConstructList:
        return AccessKind::Construct;
    default:
        return AccessKind::Unknown;
    }
}

// An operation that stands for two source operations has the origin of the later one, and names the origin of the read that comes first.
static Origin originOf(const Value& value, uint32_t part)
{
    if (!part && PayloadWord::readOrigin(value.opcode))
        return value.readOrigin();
    return value.origin();
}

static AccessSiteID accessSiteOf(const Value& value, uint32_t part)
{
    return part ? value.secondAccessSite() : value.accessSite();
}

// Nothing for the second read of a pair that reads from the result of the first: no value of the function stands for that result alone.
static ValueID receiverOperandOf(const Value& value, uint32_t part)
{
    if (value.opcode == Opcode::CallMethod)
        return value.receiverOperand();
    if (opcodeCallsByValue(value.opcode))
        return value.calleeOperand();
    if (!part)
        return value.objectOperand();
    if (value.readsFromFirstResult())
        return { };
    ValueID other = value.otherOperand();
    return other ? other : value.objectOperand();
}

static Vector<FastArm> findFastArms(const Function& function)
{
    Vector<FastArm> arms;
    auto addArmsOf = [&](const auto& instruction, AccessClass accessClass, LayoutSetID receivers) {
        if (accessClass != AccessClass::Slot && accessClass != AccessClass::DirectCall)
            return;
        forEachAccessSite(instruction, [&](AccessSiteID site) {
            if (site)
                arms.append(FastArm { site, instruction.inlineFrame, receivers });
        });
    };
    for (const Block& block : function.blocks) {
        for (ValueID id : block.values) {
            const Value& value = function.at(id);
            addArmsOf(value, accessClassOf(value.opcode), PayloadWord::receivers(value.opcode) ? value.receivers() : LayoutSetID());
        }
        addArmsOf(block.terminator, accessClassOf(block.terminator.kind), LayoutSetID());
    }
    AOT::sort(arms, { }, [](const FastArm& arm) {
        return std::tuple { arm.site, arm.inlineFrame, arm.receivers };
    });
    return arms;
}

// Each inlined copy of an access has arms of its own.
static bool addLayoutsOfFastArms(const Program& program, std::span<const FastArm> sortedArms, AccessSiteID site, uint32_t inlineFrame, LayoutList& layouts)
{
    if (!site)
        return false;
    auto arms = std::ranges::equal_range(sortedArms, std::pair { site, inlineFrame }, { }, [](const FastArm& arm) {
        return std::pair { arm.site, arm.inlineFrame };
    });
    for (const FastArm& arm : arms) {
        if (!arm.receivers)
            continue;
        for (LayoutID layout : program.at(arm.receivers).members)
            layouts.add(layout);
    }
    return !arms.empty();
}

static ArrivalSiteDescription describe(const Program& program, const Function& function, std::span<const FastArm> sortedArms, const ArrivalSite& site)
{
    const Value& value = function.at(site.value);
    ArrivalSiteDescription description {
        .operation = value.opcode,
        .origin = originOf(value, site.part),
        .accessSite = accessSiteOf(value, site.part),
        .kind = accessKindOf(value.opcode),
    };

    // The stamp of a pair is that of its first read.
    LayoutSetID stamp = site.part || opcodeCallsByValue(value.opcode) ? LayoutSetID() : value.receivers();
    if (stamp)
        description.stamp = program.at(stamp).isComplete ? ArrivalStamp::Complete : ArrivalStamp::Open;

    bool hasFastArm = addLayoutsOfFastArms(program, sortedArms, description.accessSite, description.origin.inlineFrame, description.expected);
    // Where the callee is known the read of the method is gone from the fast arm, and the call is what is left of the two.
    if (value.opcode == Opcode::CallMethod)
        hasFastArm |= addLayoutsOfFastArms(program, sortedArms, value.callAccessSite(), value.inlineFrame, description.expected);
    if (hasFastArm) {
        description.level = KnowledgeLevel::Expected;
        return description;
    }

    ValueID receiver = receiverOperandOf(value, site.part);
    if (!receiver)
        return description;
    ReceiverCandidates candidates = receiverCandidates(program, function, receiver, stamp);
    if (candidates.proof == ReceiverProof::Type) {
        description.level = KnowledgeLevel::Proven;
        description.expected = WTF::move(candidates.proven);
        return description;
    }
    description.expected = WTF::move(candidates.predicted);
    return description;
}

static Vector<ArrivalSiteDescription> describeSitesOf(const Program& program, const ArrivalSiteNumbers& numbers, FunctionID id)
{
    std::span<const ArrivalSite> sites = numbers.sitesOf(id);
    if (sites.empty())
        return { };
    const Function& function = program.at(id);
    Vector<FastArm> sortedArms = findFastArms(function);
    return Vector<ArrivalSiteDescription>(sites.size(), [&](size_t ordinal) {
        return describe(program, function, sortedArms.span(), sites[ordinal]);
    });
}

static const AccessSite* findAccessSite(const FlowResults* flow, AccessSiteID site)
{
    if (!flow || !site || site.index() >= flow->numberOfAccessSites())
        return nullptr;
    return &flow->at(site);
}

static FunctionID functionHolding(Origin origin, const Function& function, FunctionID id)
{
    if (!origin.inlineFrame || origin.inlineFrame >= function.inlineFrames.size())
        return id;
    return function.inlineFrames[origin.inlineFrame].callee;
}

static void appendSite(ArrivalSiteReport& report, ReportText& text, const PropertyAccessContext& context, FunctionID id, uint32_t number, const ArrivalSiteDescription& description)
{
    const Program& program = context.program;
    const Function& function = program.at(id);
    Wire::ArrivalSiteRecord record {
        .number = number,
        .location = function.locationOf(description.origin),
        .function = findFunctionAsRead(&program, functionHolding(description.origin, function, id)).index(),
        .operation = text.ensure(description.operation),
        .kind = description.kind,
        .level = description.level,
        .cause = UnprovenCause::Unknown,
        .firstExpectedLayout = static_cast<uint32_t>(report.siteLayouts.size()),
        .expectedLayoutCount = static_cast<uint32_t>(description.expected.size()),
        .stamp = description.stamp,
    };
    // The list of access sites in the report has these words for the same access, so the two lists agree on where it is.
    if (const AccessSite* access = findAccessSite(context.flow, description.accessSite)) {
        record.location = access->location;
        record.function = findFunctionAsRead(&program, access->function).index();
        record.kind = access->kind;
        if (description.level != KnowledgeLevel::Proven)
            record.cause = context.flow->lossOf(description.accessSite).cause;
    }
    for (LayoutID layout : description.expected)
        report.siteLayouts.append(layout.index());
    report.sites.append(record);
}

static bool isOwnDataPropertyWithTextKey(const Program& program, const Field& field)
{
    static constexpr OptionSet<FieldFlags> flagsOfOtherFields { FieldFlags::IsMethod, FieldFlags::IsPrivate, FieldFlags::IsBrand, FieldFlags::IsHidden };
    return field.key && program.at(field.key).kind == PropertyKeyKind::Text && !field.flags.containsAny(flagsOfOtherFields);
}

static void appendLayout(ArrivalSiteReport& report, ReportText& text, const Program& program, LayoutID id)
{
    const Layout& layout = program.at(id);
    uint32_t firstField = static_cast<uint32_t>(report.layoutFields.size());
    for (const Field& field : layout.fields) {
        if (!isOwnDataPropertyWithTextKey(program, field))
            continue;
        report.layoutFields.append(Wire::ArrivalLayoutFieldRecord {
            .name = text.ensure(program.at(field.key).text),
            .flags = field.flags.contains(FieldFlags::IsRequired) ? Wire::ArrivalFieldFlags { } : Wire::ArrivalFieldFlags::IsOptional,
        });
    }
    report.layouts.append(Wire::ArrivalLayoutRecord {
        .layout = id.index(),
        // Of a derived layout: the layout it was derived from.
        .base = expectedBaseOf(layout).index(),
        .kind = text.ensure(layout.kind),
        .name = text.ensure(layout.name),
        .firstField = firstField,
        .fieldCount = static_cast<uint32_t>(report.layoutFields.size()) - firstField,
        .flags = layout.isDerived() ? Wire::ArrivalLayoutFlags::IsDerived : Wire::ArrivalLayoutFlags { },
    });
}

ArrivalSiteReport describeArrivalSites(const PropertyAccessContext& context, const ArrivalSiteNumbers& numbers, BuildContext& build)
{
    ArrivalSiteReport report;
    if (!numbers.isCounting())
        return report;

    const Program& program = context.program;
    unsigned count = program.functions.size();
    Vector<Vector<ArrivalSiteDescription>> descriptionsOfFunctions(count);
    forEachIndexInParallel(build.parallelLoops(), count, [&](unsigned index) {
        descriptionsOfFunctions[index] = describeSitesOf(program, numbers, FunctionID(index));
    });

    ReportText text(report.text, program.strings);
    for (unsigned index = 0; index < count; ++index) {
        FunctionID id(index);
        const Vector<ArrivalSiteDescription>& descriptions = descriptionsOfFunctions[index];
        for (uint32_t ordinal = 0; ordinal < descriptions.size(); ++ordinal)
            appendSite(report, text, context, id, numbers.numberFor(id, ordinal), descriptions[ordinal]);
    }
    build.destroyLater(WTF::move(descriptionsOfFunctions));

    // Which layouts arrive is known after the run.
    for (unsigned index = 0; index < program.layouts.size(); ++index) {
        if (!program.layouts[index].isEnvironment())
            appendLayout(report, text, program, LayoutID(index));
    }
    return report;
}

void ArrivalSiteNumbers::dump(PrintStream& out, const Program& program) const
{
    if (!isCounting())
        return;
    for (unsigned index = 0; index < program.functions.size(); ++index) {
        FunctionID function(index);
        std::span<const ArrivalSite> sites = sitesOf(function);
        if (sites.empty())
            continue;
        Vector<ArrivalSiteDescription> descriptions = describeSitesOf(program, *this, function);
        out.print("function f", index, "\n");
        for (uint32_t ordinal = 0; ordinal < sites.size(); ++ordinal) {
            const ArrivalSiteDescription& description = descriptions[ordinal];
            out.print("v", sites[ordinal].value.index(), " site: ", numberFor(function, ordinal), " operation: ", nameOf(description.operation));
            out.print(" kind: ", nameOf(description.kind), " level: ", nameOf(description.level), " stamp: ", nameOf(description.stamp), " expected: [");
            CommaPrinter comma;
            for (LayoutID layout : description.expected)
                out.print(comma, "L", layout.index());
            out.print("]\n");
        }
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
