/*
 * Copyright (C) 2021-2025 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "ShareableBitmapUtilities.h"

#include <WebCore/HTMLVideoElement.h>
#include <WebCore/RenderImage.h>
#include <WebCore/RenderVideo.h>
#include <WebCore/ShareableBitmap.h>
#include <wtf/NativePromise.h>

namespace WebKit {
using namespace WebCore;

Ref<NativePromise<Ref<WebCore::ShareableBitmap>, void>> createShareableBitmapAsync(WebCore::RenderImage& renderImage, WebCore::CreateShareableBitmapFromImageOptions&& options)
{
#if ENABLE(VIDEO)
    if (CheckedPtr renderVideo = dynamicDowncast<RenderVideo>(renderImage))
        return protect(renderVideo->videoElement())->bitmapImageForCurrentTime();
#endif
    if (RefPtr shareableBitmap = renderImage.createShareableBitmap(options))
        return NativePromise<Ref<WebCore::ShareableBitmap>, void>::createAndResolve(shareableBitmap.releaseNonNull());
    return NativePromise<Ref<WebCore::ShareableBitmap>, void>::createAndReject();
}

}
