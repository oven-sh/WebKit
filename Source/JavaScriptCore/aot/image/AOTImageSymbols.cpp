/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTImageSymbols.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
#include "AOTCodeIndex.h"
#include "AOTFunctionDescriptions.h"
#include "AOTIRProgram.h"
#include "AOTIdentifierNumbering.h"
#include "AOTImageBuilder.h"
#include "AOTProgramTables.h"
#include "AOTRegExpRequests.h"
#include "AOTRuntimeTable.h"
#include "AOTStubDescriptor.h"
#include "AOTWasmModules.h"
#include <algorithm>
#include <tuple>
#include <wtf/BitVector.h>
#include <wtf/DataLog.h>
#include <wtf/FilePrintStream.h>
#include <wtf/HashMap.h>
#include <wtf/StringPrintStream.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringHash.h>
#include <wtf/text/StringView.h>

namespace JSC { namespace AOT {

using SymbolKind = ImageSymbol::Kind;

static constexpr size_t symbolTableHeaderWords = 4;
static constexpr size_t symbolTableEntryWords = 3;

namespace {

struct SymbolTableEntry {
    uint32_t offset;
    const String* name;
    uint32_t nameOffset { 0 };
};

} // anonymous namespace

static String escapeForMapLine(StringView text)
{
    String result = text.toString();
    for (auto [character, written] : { std::pair { "\\"_s, "\\\\"_s }, std::pair { "\t"_s, "\\t"_s }, std::pair { "\n"_s, "\\n"_s }, std::pair { "\r"_s, "\\r"_s } })
        result = makeStringByReplacingAll(result, character, written);
    return result;
}

static String textOf(const Typed::Program& program, StringID id)
{
    if (!id)
        return emptyString();
    return program.strings.toWTFString(id);
}

static String descriptionOf(const Typed::Program& program, const NumberedIdentifier& identifier)
{
    if (identifier.text)
        return textOf(program, identifier.text);
    if (!identifier.key || !program.at(identifier.key).symbol)
        return emptyString();
    BuiltinName name = program.builtinNameOf(identifier.key);
    if (!name)
        return emptyString();
    return makeString("Symbol."_s, StringView(byteCast<Latin1Character>(program.builtins().textOf(name))));
}

static const ImageHeader& headerOf(std::span<const uint8_t> image)
{
    RELEASE_ASSERT(image.size() >= sizeof(ImageHeader));
    return *reinterpret_cast<const ImageHeader*>(image.data());
}

static void forEachSymbol(std::span<const ImageSymbol> symbols, SymbolKind kind, NOESCAPE const Invocable<void(const ImageSymbol&)> auto& functor)
{
    for (const ImageSymbol& symbol : symbols) {
        if (symbol.kind == kind)
            functor(symbol);
    }
}

static String functionName(const Typed::Program& program, FunctionID id, const String& description)
{
    const Typed::Function& function = program.at(id);
    if (function.isModuleBody()) {
        if (!function.module)
            return "(module)"_s;
        return makeString("(module) "_s, textOf(program, program.at(function.module).displayPath));
    }
    String name = description.left(description.find('\t'));
    if (name.isEmpty())
        name = "(anonymous)"_s;
    if (function.isHiddenInStackTraces())
        return makeString(name, " (entry)"_s);
    return name;
}

static String stubName(StubKey key)
{
    StringPrintStream out;
    if (isOperationCallHead(key)) {
        dump(out, key.withoutImmediate());
        out.print(" to "_s, nameOf(entryAtOffset(*key.immediate())));
    } else
        dump(out, key);
    return out.toString();
}

static void appendFunctions(Vector<ImageSymbol>& symbols, const ImageSymbolInputs& inputs)
{
    const BuiltImage& built = inputs.built;
    Vector<String> descriptions = describeFunctions(inputs.program, inputs.heat);
    auto append = [&](SymbolKind kind, uint32_t position, uint32_t size, FunctionID function) {
        const String& description = descriptions[function.index()];
        symbols.append(ImageSymbol { kind, built.layout.bodyStarts[position], size, position, function, functionName(inputs.program, function, description), description });
    };

    symbols.reserveCapacity(symbols.size() + built.functions.size() + built.foldedFunctions.size());
    for (uint32_t position = 0; position < built.functions.size(); ++position) {
        const CompiledCode& code = built.functions[position];
        append(SymbolKind::Function, position, safeCast<uint32_t>(code.body.bytes.size()), code.function);
    }

    CodeIndex index(built.functions.span());
    for (const FoldedFunction& folded : built.foldedFunctions) {
        if (std::optional<uint32_t> canonical = index.indexOf(folded.canonicalFunction))
            append(SymbolKind::FoldedFunction, *canonical, folded.size, folded.function);
    }
}

static void appendSharedCodeCopies(Vector<ImageSymbol>& symbols, const CodeLayout& layout)
{
    for (uint32_t number = 0; number < layout.sharedCodeCopies.size(); ++number) {
        symbols.append(ImageSymbol {
            .kind = SymbolKind::SharedCodeCopy,
            .offset = layout.sharedCodeCopies[number],
            .size = layout.sharedCodeSize,
            .index = number,
            .name = "(stubs of compiled code)"_s,
        });
    }
}

static void appendStubs(Vector<ImageSymbol>& symbols, const StubBlob& blob)
{
    Vector<StubPlacement> placements = blob.placements;
    std::ranges::sort(placements, [](const StubPlacement& a, const StubPlacement& b) {
        return std::tie(a.offset, a.key) < std::tie(b.offset, b.key);
    });

    for (size_t first = 0; first < placements.size();) {
        uint32_t offset = placements[first].offset;
        size_t next = first;
        while (next < placements.size() && placements[next].offset == offset)
            ++next;
        uint32_t end = next < placements.size() ? placements[next].offset : safeCast<uint32_t>(blob.code.bytes.size());
        for (; first < next; ++first) {
            symbols.append(ImageSymbol {
                .kind = SymbolKind::SharedCode,
                .offset = offset,
                .size = end - offset,
                .name = stubName(placements[first].key),
            });
        }
    }
}

static void appendArgumentThunks(Vector<ImageSymbol>& symbols, const SharedCode& sharedCode)
{
    const ArgumentThunks& thunks = sharedCode.argumentThunks;
    RELEASE_ASSERT(thunks.offsets.size() == sharedCode.argumentMoves.size());
    for (size_t i = 0; i < thunks.offsets.size(); ++i) {
        uint32_t end = i + 1 < thunks.offsets.size() ? thunks.offsets[i + 1] : safeCast<uint32_t>(thunks.code.bytes.size());
        symbols.append(ImageSymbol {
            .kind = SymbolKind::SharedCode,
            .offset = sharedCode.argumentThunksOffset + thunks.offsets[i],
            .size = end - thunks.offsets[i],
            .name = makeString(stubName(StubKey::fromBits(sharedCode.argumentMoves[i]->at(0))), " with arguments"_s),
        });
    }
}

static void appendSharedOutlinedBodies(Vector<ImageSymbol>& symbols, const BuiltImage& built)
{
    const OutlinedBodies& sharedBodies = built.outlinedCode.sharedBodies;
    for (uint32_t number = 0; number < built.outlining.bodies.size(); ++number) {
        if (built.outlining.bodies[number].singleCaller)
            continue;
        uint32_t indexWithinKind = built.outlinedCode.indexWithinKind[number];
        symbols.append(ImageSymbol {
            .kind = SymbolKind::SharedCode,
            .offset = built.sharedCode.sharedBodiesOffset + sharedBodies.offsets[indexWithinKind],
            .size = safeCast<uint32_t>(sharedBodies.sizeOf(indexWithinKind)),
            .name = makeString("outlined "_s, number),
        });
    }
}

static void appendSharedCodeVeneers(Vector<ImageSymbol>& symbols, const BuiltImage& built)
{
    uint32_t veneersOffset = built.sharedCode.veneersOffset;
    if (veneersOffset >= built.layout.sharedCodeSize)
        return;
    symbols.append(ImageSymbol {
        .kind = SymbolKind::SharedCode,
        .offset = veneersOffset,
        .size = built.layout.sharedCodeSize - veneersOffset,
        .name = "veneers"_s,
    });
}

static void appendOwnOutlinedBodies(Vector<ImageSymbol>& symbols, const BuiltImage& built)
{
    const OutlinedBodies& ownBodies = built.outlinedCode.ownBodies;
    for (uint32_t number = 0; number < built.outlining.bodies.size(); ++number) {
        OptionalIndex singleCaller = built.outlining.bodies[number].singleCaller;
        if (!singleCaller)
            continue;
        uint32_t indexWithinKind = built.outlinedCode.indexWithinKind[number];
        symbols.append(ImageSymbol {
            .kind = SymbolKind::OwnOutlinedBody,
            .offset = built.layout.ownBodyOffsets[indexWithinKind],
            .size = safeCast<uint32_t>(ownBodies.sizeOf(indexWithinKind)),
            .index = *singleCaller,
            .name = makeString("outlined "_s, number),
        });
    }
}

static void appendRegExpCode(Vector<ImageSymbol>& symbols, const ImageSymbolInputs& inputs)
{
    const RegExpSection& section = inputs.built.regExpSection;
    if (section.bytes.isEmpty())
        return;

    uint32_t sectionOffset = inputs.built.layout.regExpCodeOffset;
    symbols.append(ImageSymbol {
        .kind = SymbolKind::RegExpSection,
        .offset = sectionOffset,
        .size = safeCast<uint32_t>(section.bytes.size()),
        .name = "(regular expressions)"_s,
    });

    UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> bodySizes;
    BitVector hasCode(section.entries.size());
    Vector<ImageSymbol> routines;
    for (const RegExpSectionItem& item : section.items) {
        switch (item.kind) {
        case RegExpSectionItem::Kind::Body:
            bodySizes.add(item.offset, item.size);
            hasCode.quickSet(item.regExp);
            break;
        case RegExpSectionItem::Kind::DuplicateBody:
            hasCode.quickSet(item.regExp);
            break;
        case RegExpSectionItem::Kind::Routine:
            routines.append(ImageSymbol { .kind = SymbolKind::RegExpRoutine, .offset = sectionOffset + item.offset, .size = item.size });
            break;
        case RegExpSectionItem::Kind::Table:
            break;
        }
    }

    std::span<const RegExpRequest> requests = inputs.regExpRequests.all();
    for (size_t regExp : hasCode) {
        String pattern = textOf(inputs.program, requests[regExp].pattern);
        for (uint32_t is16Bit : { 0u, 1u }) {
            uint32_t entry = section.entries[regExp][is16Bit];
            symbols.append(ImageSymbol {
                .kind = SymbolKind::RegExpBody,
                .offset = sectionOffset + entry,
                .size = bodySizes.get(entry),
                .index = safeCast<uint32_t>(regExp) << 1 | is16Bit,
                .name = pattern,
            });
        }
    }
    symbols.appendVector(WTF::move(routines));
}

static void appendWebAssemblyCode(Vector<ImageSymbol>& symbols, const CodeLayout& layout, const Typed::Program& program)
{
    if (!mayHaveWebAssemblyModules(program) || layout.webAssemblyCodeOffset >= layout.constantsOffset)
        return;
    symbols.append(ImageSymbol {
        .kind = SymbolKind::WebAssemblyCode,
        .offset = layout.webAssemblyCodeOffset,
        .size = layout.constantsOffset - layout.webAssemblyCodeOffset,
        .name = "(WebAssembly code)"_s,
    });
}

static void appendConstants(Vector<ImageSymbol>& symbols, const BuiltImage& built)
{
    if (built.constants.bytes.isEmpty())
        return;
    symbols.append(ImageSymbol {
        .kind = SymbolKind::Constants,
        .offset = built.layout.constantsOffset,
        .size = safeCast<uint32_t>(built.constants.bytes.size()),
        .name = "(constants)"_s,
    });
}

static void appendFFICode(Vector<ImageSymbol>& symbols, const CodeLayout& layout)
{
    if (layout.ffiCodeOffset >= layout.codeSize)
        return;
    symbols.append(ImageSymbol {
        .kind = SymbolKind::FFICode,
        .offset = layout.ffiCodeOffset,
        .size = safeCast<uint32_t>(layout.codeSize - layout.ffiCodeOffset),
        .name = "(FFI code)"_s,
    });
}

Vector<ImageSymbol> collectSymbols(const ImageSymbolInputs& inputs)
{
    const BuiltImage& built = inputs.built;
    Vector<ImageSymbol> symbols;
    appendFunctions(symbols, inputs);
    appendSharedCodeCopies(symbols, built.layout);
    appendStubs(symbols, built.sharedCode.blob);
    appendArgumentThunks(symbols, built.sharedCode);
    appendSharedOutlinedBodies(symbols, built);
    appendSharedCodeVeneers(symbols, built);
    appendOwnOutlinedBodies(symbols, built);
    appendRegExpCode(symbols, inputs);
    appendWebAssemblyCode(symbols, built.layout, inputs.program);
    appendConstants(symbols, built);
    appendFFICode(symbols, built.layout);
    return symbols;
}

static Vector<SymbolTableEntry> collectTableEntries(std::span<const ImageSymbol> symbols)
{
    Vector<uint32_t, 4> sharedCodeCopies;
    bool hasSharedCode = false;
    for (const ImageSymbol& symbol : symbols) {
        if (symbol.kind == SymbolKind::SharedCodeCopy)
            sharedCodeCopies.append(symbol.offset);
        hasSharedCode |= symbol.kind == SymbolKind::SharedCode;
    }

    Vector<SymbolTableEntry> entries;
    entries.reserveInitialCapacity(symbols.size());
    for (const ImageSymbol& symbol : symbols) {
        switch (symbol.kind) {
        case SymbolKind::Function:
        case SymbolKind::RegExpSection:
        case SymbolKind::WebAssemblyCode:
        case SymbolKind::Constants:
        case SymbolKind::FFICode:
            entries.append(SymbolTableEntry { symbol.offset, &symbol.name });
            break;
        case SymbolKind::SharedCode:
            for (uint32_t sharedCodeCopy : sharedCodeCopies)
                entries.append(SymbolTableEntry { sharedCodeCopy + symbol.offset, &symbol.name });
            break;
        case SymbolKind::SharedCodeCopy:
            if (!hasSharedCode)
                entries.append(SymbolTableEntry { symbol.offset, &symbol.name });
            break;
        case SymbolKind::FoldedFunction:
        case SymbolKind::OwnOutlinedBody:
        case SymbolKind::RegExpBody:
        case SymbolKind::RegExpRoutine:
            break;
        }
    }
    return entries;
}

Vector<uint8_t> encodeSymbols(std::span<const ImageSymbol> symbols, std::span<const uint8_t> image)
{
    const ImageHeader& header = headerOf(image);

    Vector<SymbolTableEntry> entries = collectTableEntries(symbols);
    std::ranges::stable_sort(entries, { }, &SymbolTableEntry::offset);
    entries.shrink(std::ranges::unique(entries, { }, &SymbolTableEntry::offset).begin() - entries.begin());

    Vector<uint8_t> names;
    UncheckedKeyHashMap<String, uint32_t> nameOffsets;
    for (SymbolTableEntry& entry : entries) {
        entry.nameOffset = nameOffsets.ensure(*entry.name, [&] {
            uint32_t offset = safeCast<uint32_t>(names.size());
            names.append(byteCast<uint8_t>(entry.name->utf8().span()));
            names.append(0);
            return offset;
        }).iterator->value;
    }

    Vector<uint8_t> result;
    auto appendWord = [&](uint32_t word) {
        for (unsigned shift = 0; shift < 32; shift += 8)
            result.append(static_cast<uint8_t>(word >> shift));
    };
    result.reserveInitialCapacity((symbolTableHeaderWords + entries.size() * symbolTableEntryWords) * sizeof(uint32_t) + names.size());
    appendWord(safeCast<uint32_t>(entries.size()));
    appendWord(safeCast<uint32_t>(names.size()));
    appendWord(safeCast<uint32_t>(header.codeOffset));
    appendWord(safeCast<uint32_t>(header.size - header.codeOffset));
    uint32_t codeSize = safeCast<uint32_t>(header.codeSize);
    for (size_t i = 0; i < entries.size(); ++i) {
        appendWord(entries[i].offset);
        appendWord((i + 1 < entries.size() ? entries[i + 1].offset : codeSize) - entries[i].offset);
        appendWord(entries[i].nameOffset);
    }
    result.appendVector(names);
    return result;
}

static void printFunctionColumns(PrintStream& out, const ImageSymbol& symbol, const Typed::Program& program)
{
    const Typed::Function& function = program.at(symbol.function);
    uint32_t module = function.module ? function.module.index() + 1 : 0;
    out.println(symbol.offset, "\t", symbol.size, "\t", module, "\t", symbol.function.index(), "\t", static_cast<unsigned>(function.role), "\t", symbol.description);
}

static void printFunctions(PrintStream& out, std::span<const ImageSymbol> symbols, const LinkedFunctions& functions, const Typed::Program& program)
{
    forEachSymbol(symbols, SymbolKind::Function, [&](const ImageSymbol& symbol) {
        const CompiledCode& code = functions[symbol.index];
        out.print("F\t", symbol.index, "\t");
        printFunctionColumns(out, symbol, program);
        if (!code.sizesByOperation.isNull())
            out.println("O\t", symbol.index, code.sizesByOperation);
    });
    forEachSymbol(symbols, SymbolKind::FoldedFunction, [&](const ImageSymbol& symbol) {
        out.print("A\t");
        printFunctionColumns(out, symbol, program);
    });
}

static void printSharedCode(PrintStream& out, std::span<const ImageSymbol> symbols, const StubBlob& blob)
{
    forEachSymbol(symbols, SymbolKind::SharedCodeCopy, [&](const ImageSymbol& symbol) {
        out.println("C\t", symbol.offset);
    });
    forEachSymbol(symbols, SymbolKind::SharedCode, [&](const ImageSymbol& symbol) {
        out.println("T\t", symbol.offset, "\t", symbol.name);
    });
    for (const StubPlacement& placement : blob.placements) {
        if (neverReturns(placement.key))
            out.println("TN\t", placement.offset);
    }
}

static void printOutlining(PrintStream& out, std::span<const ImageSymbol> symbols, const Outlining& outlining, const OutlinedCode& outlinedCode)
{
    if (!outlining.bodies.isEmpty())
        out.println("Y\tO holds offsets of the functions before outlining");
    forEachSymbol(symbols, SymbolKind::OwnOutlinedBody, [&](const ImageSymbol& symbol) {
        out.println("U\t", symbol.offset, "\t", symbol.size, "\t", symbol.index, "\t", symbol.name);
    });
    for (size_t position = 0; position < outlining.sites.size(); ++position) {
        if (outlining.sites[position].isEmpty())
            continue;
        out.print("P\t", position);
        for (const OutlinedSite& site : outlining.sites[position])
            out.print("\t", site.offset, "\t", site.length, "\t", site.body);
        out.println();
        if (outlinedCode.paddingBeforeAliases[position].isEmpty())
            continue;
        out.print("E\t", position);
        for (auto [offset, size] : outlinedCode.paddingBeforeAliases[position])
            out.print("\t", offset, "\t", size);
        out.println();
    }
}

static void printTables(PrintStream& out, std::span<const uint8_t> image, const Typed::Program& program, const IdentifierNumbering& identifiers)
{
    ImageSectionRange range = headerOf(image).sections[static_cast<size_t>(ImageSection::ProgramTables)];
    constexpr unsigned layoutsWithMessageData = 0;
    unsigned describedTypes = 0;
    if (range.size) {
        auto& tables = *reinterpret_cast<const ProgramTables*>(image.subspan(range.offset, range.size).data());
        Vector<uint32_t> typeOffsets(tables.types().subspan(1 + ProgramTables::numberOfTypeGroupWords(tables.numberOfTypes())));
        std::ranges::sort(typeOffsets);
        for (size_t i = 0; i < typeOffsets.size(); ++i)
            describedTypes += typeOffsets[i] && (!i || typeOffsets[i] != typeOffsets[i - 1]);
    }
    out.println("Z\t", range.size, "\t", layoutsWithMessageData, "\t", describedTypes);

    for (uint32_t number = 1; number < identifiers.count(); ++number)
        out.println("I\t", number, "\t", escapeForMapLine(descriptionOf(program, identifiers.at(number))));
}

static void printRegExps(PrintStream& out, std::span<const ImageSymbol> symbols, const RegExpRequests& requests, const Typed::Program& program)
{
    forEachSymbol(symbols, SymbolKind::RegExpBody, [&](const ImageSymbol& symbol) {
        const RegExpRequest& request = requests.all()[symbol.index >> 1];
        out.println("X\t", symbol.offset, "\t", symbol.size, "\t", symbol.index & 1 ? 16 : 8, "\t", request.flags.toRaw(), "\t", escapeForMapLine(textOf(program, request.pattern)));
    });
    forEachSymbol(symbols, SymbolKind::RegExpRoutine, [&](const ImageSymbol& symbol) {
        out.println("XR\t", symbol.offset, "\t", symbol.size);
    });
}

static void printConstantsAndFFICode(PrintStream& out, std::span<const ImageSymbol> symbols)
{
    forEachSymbol(symbols, SymbolKind::Constants, [&](const ImageSymbol& symbol) {
        out.println("K\t", symbol.offset, "\t", symbol.size);
    });
    forEachSymbol(symbols, SymbolKind::FFICode, [&](const ImageSymbol& symbol) {
        out.println("N\t", symbol.offset, "\t", symbol.size);
    });
}

void writeMapFile(const UTF8CString& path, std::span<const ImageSymbol> symbols, const ImageSymbolInputs& inputs, std::span<const uint8_t> image, const BuildContext& context)
{
    std::unique_ptr<FilePrintStream> map = FilePrintStream::open(path, "w"_s);
    if (!map) {
        dataLogLn("AOT: cannot write the map file ", path);
        return;
    }
    PrintStream& out = *map;
    const BuiltImage& built = inputs.built;
    printFunctions(out, symbols, built.functions, inputs.program);
    printSharedCode(out, symbols, built.sharedCode.blob);
    printOutlining(out, symbols, built.outlining, built.outlinedCode);
    out.println("R\t", built.layout.functionsEnd);
    printTables(out, image, inputs.program, context.identifierNumbering());
    printRegExps(out, symbols, inputs.regExpRequests, inputs.program);
    printConstantsAndFFICode(out, symbols);
    out.println("B\t", built.layout.codeSize);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
