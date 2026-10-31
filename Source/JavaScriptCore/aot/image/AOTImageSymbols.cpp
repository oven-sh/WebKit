/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTImageBuilder.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
#include "AOTFunctionDescriptions.h"
#include "AOTIRProgram.h"
#include "AOTIdentifierNumbering.h"
#include "AOTProgramTables.h"
#include "AOTRegExpRequests.h"
#include "AOTRuntimeTable.h"
#include "AOTSort.h"
#include "AOTStubDescriptor.h"
#include "AOTWasmModules.h"
#include <algorithm>
#include <tuple>
#include "AOTBitVector.h"
#include <wtf/DataLog.h>
#include <wtf/FilePrintStream.h>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/StringPrintStream.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringHash.h>
#include <wtf/text/StringView.h>

namespace JSC { namespace AOT {

using SymbolKind = ImageSymbol::Kind;

static String escapeForMapLine(StringView text)
{
    String result = text.toString();
    for (auto [character, written] : { std::pair { "\\"_s, "\\\\"_s }, std::pair { "\t"_s, "\\t"_s }, std::pair { "\n"_s, "\\n"_s }, std::pair { "\r"_s, "\\r"_s } })
        result = makeStringByReplacingAll(result, character, written);
    return result;
}

static String textOf(const Typed::Program& program, StringID id)
{
    return id ? program.strings.toWTFString(id) : emptyString();
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

Vector<ImageSymbol> collectSymbols(const ImageSymbolInputs& inputs)
{
    const BuiltImage& built = inputs.built;
    const CodeLayout& layout = built.layout;
    const SharedCode& sharedCode = built.sharedCode;
    Vector<ImageSymbol> symbols;
    auto add = [&](SymbolKind kind, size_t offset, size_t size, String&& name = { }, uint32_t index = 0) {
        symbols.append(ImageSymbol { kind, safeCast<uint32_t>(offset), safeCast<uint32_t>(size), index, { }, WTF::move(name) });
    };

    Vector<String> descriptions = describeFunctions(inputs.program, inputs.heat);
    auto addFunction = [&](SymbolKind kind, uint32_t position, size_t size, FunctionID function) {
        const String& description = descriptions[function.index()];
        symbols.append(ImageSymbol { kind, layout.bodyStarts[position], safeCast<uint32_t>(size), position, function, functionName(inputs.program, function, description), description });
    };
    symbols.reserveCapacity(built.functions.size() + built.foldedFunctions.size());
    for (uint32_t position = 0; position < built.functions.size(); ++position)
        addFunction(SymbolKind::Function, position, built.functions[position].body.bytes.size(), built.functions[position].function);
    CodeIndex index(built.functions.span());
    for (const FoldedFunction& folded : built.foldedFunctions) {
        if (std::optional<uint32_t> canonical = index.indexOf(folded.canonicalFunction))
            addFunction(SymbolKind::FoldedFunction, *canonical, folded.size, folded.function);
    }

    for (uint32_t number = 0; number < layout.sharedCodeCopies.size(); ++number)
        add(SymbolKind::SharedCodeCopy, layout.sharedCodeCopies[number], layout.sharedCodeSize, "(stubs of compiled code)"_s, number);

    Vector<StubPlacement> placements = sharedCode.blob.placements;
    AOT::sort(placements, [](const StubPlacement& a, const StubPlacement& b) {
        return std::tie(a.offset, a.key) < std::tie(b.offset, b.key);
    });
    for (size_t first = 0; first < placements.size();) {
        uint32_t offset = placements[first].offset;
        size_t next = first;
        while (next < placements.size() && placements[next].offset == offset)
            ++next;
        size_t end = next < placements.size() ? placements[next].offset : sharedCode.blob.code.bytes.size();
        for (; first < next; ++first)
            add(SymbolKind::SharedCode, offset, end - offset, stubName(placements[first].key));
    }

    const ArgumentThunks& thunks = sharedCode.argumentThunks;
    RELEASE_ASSERT(thunks.offsets.size() == sharedCode.argumentMoves.size());
    for (size_t i = 0; i < thunks.offsets.size(); ++i) {
        size_t end = i + 1 < thunks.offsets.size() ? thunks.offsets[i + 1] : thunks.code.bytes.size();
        add(SymbolKind::SharedCode, sharedCode.argumentThunksOffset + thunks.offsets[i], end - thunks.offsets[i], makeString(stubName(StubKey::fromBits(sharedCode.argumentMoves[i]->at(0))), " with arguments"_s));
    }

    const Vector<OutlinedBody>& outlinedBodies = built.outlining.bodies;
    const OutlinedCode& outlinedCode = built.outlinedCode;
    for (uint32_t number = 0; number < outlinedBodies.size(); ++number) {
        if (outlinedBodies[number].singleCaller)
            continue;
        uint32_t indexWithinKind = outlinedCode.indexWithinKind[number];
        add(SymbolKind::SharedCode, sharedCode.sharedBodiesOffset + outlinedCode.sharedBodies.offsets[indexWithinKind], outlinedCode.sharedBodies.sizeOf(indexWithinKind), makeString("outlined "_s, number));
    }
    if (sharedCode.veneersOffset < layout.sharedCodeSize)
        add(SymbolKind::SharedCode, sharedCode.veneersOffset, layout.sharedCodeSize - sharedCode.veneersOffset, "veneers"_s);
    for (uint32_t number = 0; number < outlinedBodies.size(); ++number) {
        OptionalIndex singleCaller = outlinedBodies[number].singleCaller;
        if (!singleCaller)
            continue;
        uint32_t indexWithinKind = outlinedCode.indexWithinKind[number];
        add(SymbolKind::OwnOutlinedBody, layout.ownBodyOffsets[indexWithinKind], outlinedCode.ownBodies.sizeOf(indexWithinKind), makeString("outlined "_s, number), *singleCaller);
    }

    if (const RegExpSection& section = built.regExpSection; !section.bytes.isEmpty()) {
        add(SymbolKind::RegExpSection, layout.regExpCodeOffset, section.bytes.size(), "(regular expressions)"_s);
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
                routines.append(ImageSymbol { .kind = SymbolKind::RegExpRoutine, .offset = layout.regExpCodeOffset + item.offset, .size = item.size });
                break;
            case RegExpSectionItem::Kind::Table:
                break;
            }
        }
        for (size_t regExp : hasCode) {
            String pattern = textOf(inputs.program, inputs.regExpRequests.all()[regExp].pattern);
            for (uint32_t is16Bit : { 0u, 1u }) {
                uint32_t entry = section.entries[regExp][is16Bit];
                add(SymbolKind::RegExpBody, layout.regExpCodeOffset + entry, bodySizes.get(entry), String(pattern), safeCast<uint32_t>(regExp) << 1 | is16Bit);
            }
        }
        symbols.appendVector(WTF::move(routines));
    }

    if (mayHaveWebAssemblyModules(inputs.program) && layout.webAssemblyCodeOffset < layout.ffiCodeOffset)
        add(SymbolKind::WebAssemblyCode, layout.webAssemblyCodeOffset, layout.ffiCodeOffset - layout.webAssemblyCodeOffset, "(WebAssembly code)"_s);
    if (!built.constants.bytes.isEmpty())
        add(SymbolKind::Constants, layout.constantsOffset, built.constants.bytes.size(), "(constants)"_s);
    if (layout.ffiCodeSize)
        add(SymbolKind::FFICode, layout.ffiCodeOffset, layout.ffiCodeSize, "(FFI code)"_s);
    return symbols;
}

Vector<uint8_t> encodeSymbols(std::span<const ImageSymbol> symbols, std::span<const uint8_t> image)
{
    struct Entry {
        uint32_t offset;
        const String* name;
        uint32_t nameOffset { 0 };
    };
    Vector<uint32_t, 4> sharedCodeCopies;
    bool hasSharedCode = false;
    for (const ImageSymbol& symbol : symbols) {
        if (symbol.kind == SymbolKind::SharedCodeCopy)
            sharedCodeCopies.append(symbol.offset);
        hasSharedCode |= symbol.kind == SymbolKind::SharedCode;
    }

    Vector<Entry> entries;
    entries.reserveInitialCapacity(symbols.size());
    for (const ImageSymbol& symbol : symbols) {
        switch (symbol.kind) {
        case SymbolKind::Function:
        case SymbolKind::RegExpSection:
        case SymbolKind::WebAssemblyCode:
        case SymbolKind::Constants:
        case SymbolKind::FFICode:
            entries.append(Entry { symbol.offset, &symbol.name });
            break;
        case SymbolKind::SharedCode:
            for (uint32_t sharedCodeCopy : sharedCodeCopies)
                entries.append(Entry { sharedCodeCopy + symbol.offset, &symbol.name });
            break;
        case SymbolKind::SharedCodeCopy:
            if (!hasSharedCode)
                entries.append(Entry { symbol.offset, &symbol.name });
            break;
        case SymbolKind::FoldedFunction:
        case SymbolKind::OwnOutlinedBody:
        case SymbolKind::RegExpBody:
        case SymbolKind::RegExpRoutine:
            break;
        }
    }
    AOT::stableSort(entries, { }, &Entry::offset);
    entries.shrink(std::ranges::unique(entries, { }, &Entry::offset).begin() - entries.begin());

    Vector<uint8_t> names;
    UncheckedKeyHashMap<String, uint32_t> nameOffsets;
    for (Entry& entry : entries) {
        entry.nameOffset = nameOffsets.ensure(*entry.name, [&] {
            uint32_t offset = safeCast<uint32_t>(names.size());
            names.append(byteCast<uint8_t>(entry.name->utf8().span()));
            names.append(0);
            return offset;
        }).iterator->value;
    }

    const ImageHeader& header = headerOf(image);
    Vector<uint8_t> result;
    auto appendWord = [&](uint64_t word) {
        for (unsigned shift = 0; shift < 32; shift += 8)
            result.append(static_cast<uint8_t>(safeCast<uint32_t>(word) >> shift));
    };
    result.reserveInitialCapacity((4 + entries.size() * 3) * sizeof(uint32_t) + names.size());
    appendWord(entries.size());
    appendWord(names.size());
    appendWord(header.codeOffset);
    appendWord(header.size - header.codeOffset);
    for (size_t i = 0; i < entries.size(); ++i) {
        appendWord(entries[i].offset);
        appendWord((i + 1 < entries.size() ? entries[i + 1].offset : safeCast<uint32_t>(header.codeSize)) - entries[i].offset);
        appendWord(entries[i].nameOffset);
    }
    result.appendVector(names);
    return result;
}

static unsigned countDescribedTypes(std::span<const uint8_t> tableBytes)
{
    if (tableBytes.empty())
        return 0;
    auto& tables = *reinterpret_cast<const ProgramTables*>(tableBytes.data());
    Vector<uint32_t> typeOffsets(tables.types().subspan(1 + ProgramTables::numberOfTypeGroupWords(tables.numberOfTypes())));
    AOT::sort(typeOffsets);
    unsigned describedTypes = 0;
    for (size_t i = 0; i < typeOffsets.size(); ++i)
        describedTypes += typeOffsets[i] && (!i || typeOffsets[i] != typeOffsets[i - 1]);
    return describedTypes;
}

void writeMapFile(const UTF8CString& path, std::span<const ImageSymbol> symbols, const ImageSymbolInputs& inputs, std::span<const uint8_t> image, const BuildContext& context)
{
    // A build with several entry points compiles an image for each: the first map has the name that was asked for, the next ones that name and ".1", ".2" ..
    String name = String::fromUTF8(path.span());
    unsigned number = [&] {
        static Lock lock;
        static NeverDestroyed<HashMap<String, unsigned>> mapsWritten;
        Locker locker { lock };
        return mapsWritten->add(name, 0).iterator->value++;
    }();
    std::unique_ptr<FilePrintStream> map = FilePrintStream::open((number ? makeString(name, '.', number) : name).utf8(), "wb"_s);
    if (!map) {
        dataLogLn("AOT: cannot write the map file ", path);
        return;
    }
    PrintStream& out = *map;
    const BuiltImage& built = inputs.built;
    const Typed::Program& program = inputs.program;
    auto forEachSymbol = [&](SymbolKind kind, const auto& functor) {
        for (const ImageSymbol& symbol : symbols) {
            if (symbol.kind == kind)
                functor(symbol);
        }
    };
    auto printFunctionColumns = [&](const ImageSymbol& symbol) {
        const Typed::Function& function = program.at(symbol.function);
        out.println(symbol.offset, "\t", symbol.size, "\t", function.module ? function.module.index() + 1 : 0, "\t", symbol.function.index(), "\t", static_cast<unsigned>(function.role), "\t", symbol.description);
    };
    auto printOffsetAndSize = [&](SymbolKind kind, ASCIILiteral letters) {
        forEachSymbol(kind, [&](const ImageSymbol& symbol) {
            out.println(letters, "\t", symbol.offset, "\t", symbol.size);
        });
    };

    forEachSymbol(SymbolKind::Function, [&](const ImageSymbol& symbol) {
        out.print("F\t", symbol.index, "\t");
        printFunctionColumns(symbol);
        if (const String& sizes = built.functions[symbol.index].sizesByOperation; !sizes.isNull())
            out.println("O\t", symbol.index, sizes);
    });
    forEachSymbol(SymbolKind::FoldedFunction, [&](const ImageSymbol& symbol) {
        out.print("A\t");
        printFunctionColumns(symbol);
    });

    forEachSymbol(SymbolKind::SharedCodeCopy, [&](const ImageSymbol& symbol) {
        out.println("C\t", symbol.offset);
    });
    forEachSymbol(SymbolKind::SharedCode, [&](const ImageSymbol& symbol) {
        out.println("T\t", symbol.offset, "\t", symbol.name);
    });
    for (const StubPlacement& placement : built.sharedCode.blob.placements) {
        if (neverReturns(placement.key))
            out.println("TN\t", placement.offset);
    }

    if (!built.outlining.bodies.isEmpty())
        out.println("Y\tO holds offsets of the functions before outlining");
    forEachSymbol(SymbolKind::OwnOutlinedBody, [&](const ImageSymbol& symbol) {
        out.println("U\t", symbol.offset, "\t", symbol.size, "\t", symbol.index, "\t", symbol.name);
    });
    for (size_t position = 0; position < built.outlining.sites.size(); ++position) {
        if (built.outlining.sites[position].isEmpty())
            continue;
        out.print("P\t", position);
        for (const OutlinedSite& site : built.outlining.sites[position])
            out.print("\t", site.offset, "\t", site.length, "\t", site.body);
        out.println();
        if (built.outlinedCode.paddingBeforeAliases[position].isEmpty())
            continue;
        out.print("E\t", position);
        for (auto [offset, size] : built.outlinedCode.paddingBeforeAliases[position])
            out.print("\t", offset, "\t", size);
        out.println();
    }
    out.println("R\t", built.layout.functionsEnd);

    ImageSectionRange tables = headerOf(image).sections[static_cast<size_t>(ImageSection::ProgramTables)];
    out.println("Z\t", tables.size, "\t0\t", countDescribedTypes(image.subspan(tables.offset, tables.size)));
    const IdentifierNumbering& identifiers = context.identifierNumbering();
    for (uint32_t identifier = 1; identifier < identifiers.count(); ++identifier)
        out.println("I\t", identifier, "\t", escapeForMapLine(descriptionOf(program, identifiers.at(identifier))));

    forEachSymbol(SymbolKind::RegExpBody, [&](const ImageSymbol& symbol) {
        out.println("X\t", symbol.offset, "\t", symbol.size, "\t", symbol.index & 1 ? 16 : 8, "\t", inputs.regExpRequests.all()[symbol.index >> 1].flags.toRaw(), "\t", escapeForMapLine(symbol.name));
    });
    printOffsetAndSize(SymbolKind::RegExpRoutine, "XR"_s);
    printOffsetAndSize(SymbolKind::Constants, "K"_s);
    printOffsetAndSize(SymbolKind::FFICode, "N"_s);
    if (!inputs.arrivalSites.isNull())
        out.print(inputs.arrivalSites);
    out.println("B\t", built.layout.codeSize);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
