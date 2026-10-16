/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
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
#include "ProofOfPossessionRequest.h"

#if ENABLE(PROOF_OF_POSSESSION)

#include "Exception.h"

namespace WebCore {

// Upper bounds that keep the IPC payload small. Callers apply any stricter rules before making a request.
static constexpr unsigned maximumSubjectHintLength = 15;
static constexpr size_t maximumIdentifierHintLength = 64;
static constexpr size_t maximumIssuerPublicKeyLength = 1024;

std::optional<Exception> validateProofOfPossessionRequest(const ProofOfPossessionRequest& request)
{
    if (!request.challenge.byteLength())
        return Exception { ExceptionCode::TypeError, "The challenge must not be empty."_s };

    if (!request.subjectHint.isNull() && (request.subjectHint.isEmpty() || request.subjectHint.length() > maximumSubjectHintLength))
        return Exception { ExceptionCode::TypeError, "The subject hint must contain between 1 and 15 characters."_s };

    if (request.identifierHint && (!request.identifierHint->byteLength() || request.identifierHint->byteLength() > maximumIdentifierHintLength))
        return Exception { ExceptionCode::TypeError, "The identifier hint must be between 1 and 64 bytes long."_s };

    if (request.knownIssuerPublicKey && (!request.knownIssuerPublicKey->byteLength() || request.knownIssuerPublicKey->byteLength() > maximumIssuerPublicKeyLength))
        return Exception { ExceptionCode::TypeError, "The known issuer public key must be between 1 and 1024 bytes long."_s };

    return std::nullopt;
}

} // namespace WebCore

#endif // ENABLE(PROOF_OF_POSSESSION)
