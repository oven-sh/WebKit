/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTImage.h"
#include "AOTImageFormat.h"
#include "AOTVector.h"
#include <memory>
#include <optional>
#include <span>
#include <wtf/CheckedArithmetic.h>
#include <wtf/FastMalloc.h>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>
#include <wtf/text/StringHash.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT {

struct DecodedString;

// CompileOptions::imagesBelow, read. The image that is built goes on where the last of them ends, and a name that one of them has keeps its number.
class ImagesBelow {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(ImagesBelow);
    WTF_MAKE_NONCOPYABLE(ImagesBelow);
public:
    // Null: the bytes are no images of this compiler that share their realms.
    static std::unique_ptr<ImagesBelow> tryCreate(std::span<const std::span<const uint8_t>> images);

    unsigned count() const { return m_images.size(); }
    bool hasPrebuiltLibrary() const { return m_images.first()->holdsLibraryAlone(); }
    const ImageNumberRanges& numbersOfLast() const { return m_images.last()->header().numbers; }
    uint32_t capacityOfStringHeaders() const { return m_images.first()->header().numbers.capacityOfStringHeaders; }
    // Image::layoutIndicesBelow() of the image that is built.
    uint32_t layoutIndices() const
    {
        CheckedUint32 sum = 0;
        for (const std::unique_ptr<Image>& image : m_images)
            sum += image->programTables().numberOfLayoutIndices;
        return sum;
    }
    // What one of them leaves open, for its tests or for a write of its own, no image over it locks or takes for a constant.
    void forEachUnlockedBuiltinMember(NOESCAPE const Invocable<void(BuiltinID)> auto& functor) const
    {
        for (const std::unique_ptr<Image>& image : m_images) {
            for (uint32_t member : image->programTables().unlockedBuiltinMembers())
                functor(BuiltinID(member & ~ProgramTables::unlockedBuiltinMember));
        }
    }

    std::optional<uint32_t> numberOfName(const DecodedString&) const;
    // The text of a name by its number. Null for a symbol or a private name.
    StringView textOfName(uint32_t number) const;

    // What the code of one of them makes with slots: a record, or the instances of a class. Its objects come to the code that is built as any value does.
    struct Record {
        unsigned image { 0 };
        uint32_t firstShape { 0 };
        uint32_t numberOfShapes { 0 };
        const LayoutData* layout { nullptr };
    };
    void forEachRecord(NOESCAPE const Invocable<void(const Record&)> auto& functor) const
    {
        for (unsigned position = 0; position < m_images.size(); ++position) {
            const ProgramTables& tables = m_images[position]->programTables();
            const ImageNumberRanges& numbers = m_images[position]->header().numbers;
            Record record;
            auto finish = [&] {
                if (record.layout)
                    functor(record);
                record = { };
            };
            for (uint32_t shape = numbers.shapesBelow + 1; shape <= numbers.lastShape && shape < tables.firstEnvironmentShape; ++shape) {
                const LayoutData* layout = tables.hasLayoutForShape(shape) ? &tables.layoutForShape(shape) : nullptr;
                if (layout && record.layout && layout->index == record.layout->index) {
                    ++record.numberOfShapes;
                    continue;
                }
                finish();
                if (layout && (layout->kind() == LayoutKind::Record || layout->kind() == LayoutKind::Instance))
                    record = { position, shape, 1, layout };
            }
            finish();
        }
    }
    Record recordAt(unsigned image, uint32_t firstShape, uint32_t numberOfShapes) const { return { image, firstShape, numberOfShapes, &m_images[image]->programTables().layoutForShape(firstShape) }; }
    // Of a shape of any of them. Nothing for what is neither a record nor an instance of a class.
    std::optional<Record> recordWithShape(uint32_t shape) const
    {
        for (unsigned position = 0; position < m_images.size(); ++position) {
            const ProgramTables& tables = m_images[position]->programTables();
            const ImageNumberRanges& numbers = m_images[position]->header().numbers;
            if (shape <= numbers.shapesBelow || shape > numbers.lastShape)
                continue;
            if (!tables.hasLayoutForShape(shape))
                return std::nullopt;
            const LayoutData& layout = tables.layoutForShape(shape);
            if (layout.kind() != LayoutKind::Record && layout.kind() != LayoutKind::Instance)
                return std::nullopt;
            auto isOfLayout = [&](uint32_t other) {
                return other > numbers.shapesBelow && other <= numbers.lastShape && tables.hasLayoutForShape(other) && tables.layoutForShape(other).index == layout.index;
            };
            uint32_t first = shape;
            while (isOfLayout(first - 1))
                --first;
            uint32_t end = shape + 1;
            while (isOfLayout(end))
                ++end;
            return Record { position, first, end - first, &tables.layoutForShape(first) };
        }
        return std::nullopt;
    }
    // The shapes that a test of the image of the record passes, ascending.
    void forEachShapeIn(const Record& record, const RuntimeCellTest& test, NOESCAPE const Invocable<void(uint32_t)> auto& functor) const
    {
        const ProgramTables& tables = m_images[record.image]->programTables();
        const ShapeTestData& shapes = tables.shapeTestAt(test.shapeTestOffset());
        for (uint32_t shape = shapes.first(); shapes.count && shape <= shapes.last; ++shape) {
            if (tables.passes(shapes, shape))
                functor(shape);
        }
    }
    const RuntimeType& typeOf(const Record& record, const FieldData& field) const { return m_images[record.image]->programTables().typeAt(field.type); }
    // The fields, and the members of the prototype object of its class and of the classes above that one which the image knows. A name may come twice.
    void forEachNameOf(const Record& record, NOESCAPE const Invocable<void(uint32_t)> auto& functor) const
    {
        for (const FieldData& field : record.layout->fields())
            functor(field.name);
        const ProgramTables& tables = m_images[record.image]->programTables();
        const LayoutData* current = record.layout;
        for (uint32_t remaining = tables.numberOfLayoutIndices; current->isClass() && remaining--;) {
            for (const MethodData& method : current->methods())
                functor(method.name);
            uint32_t base = current->assumedBase();
            uint32_t offset = base < tables.numberOfLayoutIndices ? tables.offsetOfClass(base) : 0;
            if (!offset)
                return;
            current = tables.at<LayoutData>(offset);
        }
    }
    bool findsName(const Record& record, uint32_t number) const
    {
        bool isFound = false;
        forEachNameOf(record, [&](uint32_t name) {
            isFound |= name == number;
        });
        return isFound;
    }
    // Whether a file with code at the path is a module of one of them, entry point or not.
    bool holdsFile(StringView path) const;
    // Of a key that two images make the same symbol of: a well-known or a registered symbol, a private name of the engine.
    std::optional<uint32_t> numberOfEngineSymbol(PropertyKeyKind, uint32_t wellKnownSymbol, const String& text) const;

private:
    ImagesBelow() = default;
    StringView viewOf(size_t position, StringID) const;

    Vector<std::unique_ptr<Image>, 2> m_images;
    UncheckedKeyHashMap<String, uint32_t> m_engineSymbols;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
