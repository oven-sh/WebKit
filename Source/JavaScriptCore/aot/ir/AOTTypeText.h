/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTTypeArena.h"
#include <optional>
#include <wtf/PrintStream.h>
#include <wtf/text/StringView.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT { namespace Typed {

struct TypeTextOptions {
    static constexpr uint32_t everyMember = UINT32_MAX;

    uint32_t mostMembersPrinted { everyMember };
    bool printsSeeds { true };
    bool namesFamiliesOfLayouts { false };
};

void printType(PrintStream&, TypeReader, TypeID, TypeTextOptions = { });
void printObject(PrintStream&, TypeReader, ObjectID);
void printLayoutSet(PrintStream&, TypeReader, LayoutSetID);
void printObjectSet(PrintStream&, TypeReader, ObjectSetID, TypeTextOptions = { });

struct TypeInText {
    TypeReader types;
    TypeID type;

    void dump(PrintStream& out) const { printType(out, types, type); }
};

std::optional<TypeID> parseType(TypeArena&, StringView text, unsigned& offset, String& error);
std::optional<ObjectID> parseObject(const TypeArena&, StringView text, unsigned& offset, String& error);
std::optional<LayoutSetID> parseLayoutSet(TypeArena&, StringView text, unsigned& offset, String& error);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
