/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTSchemaEnums.h"
#include "AOTTypeIDs.h"
#include "AOTWire.h"
#include <memory>
#include <optional>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

class LinkFlow;

struct TypeOrigin {
    Wire::TypeOriginReason reason { Wire::TypeOriginReason::Unknown };
    Wire::SourceLocation location;
    Wire::SourceLocation entry;
    StringID name;
    BuiltinID builtin;
    std::optional<uint32_t> node;
};

enum class OriginUser : uint8_t { Access, Call, Operator };

class TypeOrigins {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(TypeOrigins);
public:
    struct User {
        Wire::SourceLocation location;
        FunctionID function;
        OriginUser kind { OriginUser::Access };
        uint32_t origin { 0 };
        bool hasOtherOrigins { false };
    };

    const User* find(const Wire::SourceLocation&, FunctionID, OriginUser) const;

    Vector<TypeOrigin> origins;
    Vector<User> users;
};

std::unique_ptr<TypeOrigins> traceTypeOrigins(const LinkFlow&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
