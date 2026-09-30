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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

// lib/convert-cjk-codecs.py made this from Modules/cjkcodecs/_codecs_tw.c of CPython 3.14. It is not to be changed by hand.

#include "config.h"

/*
 * _codecs_tw.c: Codecs collection for Taiwan's encodings
 *
 * Written by Hye-Shik Chang <perky@FreeBSD.org>
 */

#include "PythonCJKCodecs.h"
#include "PythonCJKMappingsTW.h"

/*
 * BIG5 codec
 */

ENCODER(big5)
{
    while (*inpos < inlen) {
        Py_UCS4 c = INCHAR1;
        DBCHAR code;

        if (c < 0x80) {
            REQUIRE_OUTBUF(1);
            **outbuf = (unsigned char)c;
            NEXT(1, 1);
            continue;
        }

        if (c > 0xFFFF)
            return 1;

        REQUIRE_OUTBUF(2);

        if (TRYMAP_ENC(big5, code, c))
            ;
        else
            return 1;

        OUTBYTE1(code >> 8);
        OUTBYTE2(code & 0xFF);
        NEXT(1, 2);
    }

    return 0;
}

DECODER(big5)
{
    while (inleft > 0) {
        unsigned char c = INBYTE1;
        Py_UCS4 decoded;

        if (c < 0x80) {
            OUTCHAR(c);
            NEXT_IN(1);
            continue;
        }

        REQUIRE_INBUF(2);
        if (TRYMAP_DEC(big5, decoded, c, INBYTE2)) {
            OUTCHAR(decoded);
            NEXT_IN(2);
        }
        else return 1;
    }

    return 0;
}


/*
 * CP950 codec
 */

ENCODER(cp950)
{
    while (*inpos < inlen) {
        Py_UCS4 c = INCHAR1;
        DBCHAR code;

        if (c < 0x80) {
            WRITEBYTE1((unsigned char)c);
            NEXT(1, 1);
            continue;
        }

        if (c > 0xFFFF)
            return 1;

        REQUIRE_OUTBUF(2);
        if (TRYMAP_ENC(cp950ext, code, c))
            ;
        else if (TRYMAP_ENC(big5, code, c))
            ;
        else
            return 1;

        OUTBYTE1(code >> 8);
        OUTBYTE2(code & 0xFF);
        NEXT(1, 2);
    }

    return 0;
}

DECODER(cp950)
{
    while (inleft > 0) {
        unsigned char c = INBYTE1;
        Py_UCS4 decoded;

        if (c < 0x80) {
            OUTCHAR(c);
            NEXT_IN(1);
            continue;
        }

        REQUIRE_INBUF(2);

        if (TRYMAP_DEC(cp950ext, decoded, c, INBYTE2))
            OUTCHAR(decoded);
        else if (TRYMAP_DEC(big5, decoded, c, INBYTE2))
            OUTCHAR(decoded);
        else
            return 1;

        NEXT_IN(2);
    }

    return 0;
}



BEGIN_MAPPINGS_LIST(2)
  MAPPING_ENCDEC(big5)
  MAPPING_ENCDEC(cp950ext)
END_MAPPINGS_LIST

BEGIN_CODECS_LIST(2)
  CODEC_STATELESS(big5)
  CODEC_STATELESS(cp950)
END_CODECS_LIST

I_AM_A_MODULE_FOR(tw, TW)

} } } // namespace JSC::Python::(anonymous)
