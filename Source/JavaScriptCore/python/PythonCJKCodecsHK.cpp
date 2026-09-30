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

// lib/convert-cjk-codecs.py made this from Modules/cjkcodecs/_codecs_hk.c of CPython 3.14. It is not to be changed by hand.

#include "config.h"

/*
 * _codecs_hk.c: Codecs collection for encodings from Hong Kong
 *
 * Written by Hye-Shik Chang <perky@FreeBSD.org>
 */

#define USING_IMPORTED_MAPS

#define CJK_MOD_SPECIFIC_STATE      \
    const encode_map *big5_encmap;  \
    const decode_map *big5_decmap;

#include "PythonCJKCodecs.h"
#include "PythonCJKMappingsHK.h"

/*
 * BIG5HKSCS codec
 */

CODEC_INIT(big5hkscs)
{
    cjkcodecs_module_state *st = MODSTATE(codec);
    if (IMPORT_MAP(tw, big5, &st->big5_encmap, &st->big5_decmap)) {
        return -1;
    }
    return 0;
}

/*
 * There are four possible pair unicode -> big5hkscs maps as in HKSCS 2004:
 *  U+00CA U+0304 -> 8862  (U+00CA alone is mapped to 8866)
 *  U+00CA U+030C -> 8864
 *  U+00EA U+0304 -> 88a3  (U+00EA alone is mapped to 88a7)
 *  U+00EA U+030C -> 88a5
 * These are handled by not mapping tables but a hand-written code.
 */
static const DBCHAR big5hkscs_pairenc_table[4] = {0x8862, 0x8864, 0x88a3, 0x88a5};

ENCODER(big5hkscs)
{
    while (*inpos < inlen) {
        Py_UCS4 c = INCHAR1;
        DBCHAR code;
        Py_ssize_t insize;

        if (c < 0x80) {
            REQUIRE_OUTBUF(1);
            **outbuf = (unsigned char)c;
            NEXT(1, 1);
            continue;
        }

        insize = 1;
        REQUIRE_OUTBUF(2);

        if (c < 0x10000) {
            if (TRYMAP_ENC(big5hkscs_bmp, code, c)) {
                if (code == MULTIC) {
                    Py_UCS4 c2;
                    if (inlen - *inpos >= 2)
                        c2 = INCHAR2;
                    else
                        c2 = 0;

                    if (inlen - *inpos >= 2 &&
                        ((c & 0xffdf) == 0x00ca) &&
                        ((c2 & 0xfff7) == 0x0304)) {
                        code = big5hkscs_pairenc_table[
                            ((c >> 4) |
                             (c2 >> 3)) & 3];
                        insize = 2;
                    }
                    else if (inlen - *inpos < 2 &&
                             !(flags & MBENC_FLUSH))
                        return MBERR_TOOFEW;
                    else {
                        if (c == 0xca)
                            code = 0x8866;
                        else /* c == 0xea */
                            code = 0x88a7;
                    }
                }
            }
            else if (TRYMAP_ENC_ST(big5, code, c))
                ;
            else
                return 1;
        }
        else if (c < 0x20000)
            return insize;
        else if (c < 0x30000) {
            if (TRYMAP_ENC(big5hkscs_nonbmp, code, c & 0xffff))
                ;
            else
                return insize;
        }
        else
            return insize;

        OUTBYTE1(code >> 8);
        OUTBYTE2(code & 0xFF);
        NEXT(insize, 2);
    }

    return 0;
}

#define BH2S(c1, c2) (((c1) - 0x87) * (0xfe - 0x40 + 1) + ((c2) - 0x40))

DECODER(big5hkscs)
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

        if (0xc6 > c || c > 0xc8 || (c < 0xc7 && INBYTE2 < 0xa1)) {
            if (TRYMAP_DEC_ST(big5, decoded, c, INBYTE2)) {
                OUTCHAR(decoded);
                NEXT_IN(2);
                continue;
            }
        }

        if (TRYMAP_DEC(big5hkscs, decoded, c, INBYTE2))
        {
            int s = BH2S(c, INBYTE2);
            const unsigned char *hintbase;

            ASSERT(0x87 <= c && c <= 0xfe);
            ASSERT(0x40 <= INBYTE2 && INBYTE2 <= 0xfe);

            if (BH2S(0x87, 0x40) <= s && s <= BH2S(0xa0, 0xfe)) {
                    hintbase = big5hkscs_phint_0;
                    s -= BH2S(0x87, 0x40);
            }
            else if (BH2S(0xc6,0xa1) <= s && s <= BH2S(0xc8,0xfe)){
                    hintbase = big5hkscs_phint_12130;
                    s -= BH2S(0xc6, 0xa1);
            }
            else if (BH2S(0xf9,0xd6) <= s && s <= BH2S(0xfe,0xfe)){
                    hintbase = big5hkscs_phint_21924;
                    s -= BH2S(0xf9, 0xd6);
            }
            else
                    return MBERR_INTERNAL;

            if (hintbase[s >> 3] & (1 << (s & 7))) {
                    OUTCHAR(decoded | 0x20000);
                    NEXT_IN(2);
            }
            else {
                    OUTCHAR(decoded);
                    NEXT_IN(2);
            }
            continue;
        }

        switch ((c << 8) | INBYTE2) {
        case 0x8862: OUTCHAR2(0x00ca, 0x0304); break;
        case 0x8864: OUTCHAR2(0x00ca, 0x030c); break;
        case 0x88a3: OUTCHAR2(0x00ea, 0x0304); break;
        case 0x88a5: OUTCHAR2(0x00ea, 0x030c); break;
        default: return 1;
        }

        NEXT_IN(2); /* all decoded code points are pairs, above. */
    }

    return 0;
}

BEGIN_MAPPINGS_LIST(3)
  MAPPING_DECONLY(big5hkscs)
  MAPPING_ENCONLY(big5hkscs_bmp)
  MAPPING_ENCONLY(big5hkscs_nonbmp)
END_MAPPINGS_LIST

BEGIN_CODECS_LIST(1)
  CODEC_STATELESS_WINIT(big5hkscs)
END_CODECS_LIST

I_AM_A_MODULE_FOR(hk, HK)

} } } // namespace JSC::Python::(anonymous)
