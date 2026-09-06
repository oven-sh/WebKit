/*
 * Copyright (C) 2012 University of Szeged. All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 */

#pragma once

#include <wtf/ExportMacros.h>
#include <wtf/Platform.h>

namespace WTF {

// This counts logical cores. The result is computed once and cached.
WTF_EXPORT_PRIVATE int numberOfProcessorCores();

// Same count, read from the OS on every call. On Linux it follows a change to the
// affinity mask or to the cgroup cpu quota made after startup, as libuv's
// uv_available_parallelism() does.
WTF_EXPORT_PRIVATE int numberOfProcessorCoresUncached();

#if OS(DARWIN)
WTF_EXPORT_PRIVATE int numberOfPhysicalProcessorCores();
#endif

}
