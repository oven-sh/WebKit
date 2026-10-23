/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTChildProcess.h"
#include "AOTDataBuilder.h"
#include "AOTImage.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTObject.h"
#include "AOTOperationData.h"
#include "AOTProgramStatics.h"
#include "AOTProgramStrings.h"
#include "AOTPropertyWalk.h"
#include "AOTTemporaryFile.h"
#include "AOTVector.h"
#include "JSArrayInlines.h"
#include "JSBigInt.h"
#include "JSCInlines.h"
#include "JSCellButterfly.h"
#include "Options.h"
#include "TopExceptionScope.h"
#include <wtf/DataLog.h>
#include <wtf/HashSet.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC::AOT {

std::span<const NativeTest> dataBuilderTests();

static constexpr ASCIILiteral collectionInTheMiddleTest = "data-builder/collection-in-the-middle-loses-nothing"_s;

static constexpr ASCIILiteral programText = R"IR(layout L0 name="Outer" fields=[{key="mixed" declared={type=anything} flags=[IsRequired]}, {key="doubles" declared={type=anything} flags=[IsRequired]}, {key="integers" declared={type=anything} flags=[IsRequired]}, {key="inner" declared={type=anything} flags=[IsRequired]}, {key="big" declared={type=anything} flags=[IsRequired]}, {key="text" declared={type=anything} flags=[IsRequired]}, {key="nested" declared={type=anything} flags=[IsRequired]}]
layout L1 name="Inner" fields=[{key="zero" declared={type=number} flags=[IsRequired]}, {key="nan" declared={type=number} flags=[IsRequired]}, {key="nothing" declared={type=anything} flags=[IsRequired]}]
layout L2 name="Dictionary" supertypes=[L5]
layout L3 name="Named" fields=[{key="name" declared={type=string} flags=[IsRequired]}, {key="size" declared={type=number} flags=[IsRequired]}] supertypes=[L6]
layout L4 name="SeenByUnknownCodeOnly" fields=[{key="first" declared={type=number} flags=[IsRequired]}] supertypes=[L7]
layout L5 name="DeclaredDictionaryType"
layout L6 name="DeclaredNamedType" fields=[{key="name" declared={type=string} flags=[IsRequired]}, {key="size" declared={type=number} flags=[IsRequired]}]
layout L7 name="DeclaredTypeOfFirst" fields=[{key="first" declared={type=number} flags=[IsRequired]}]
module m0 path="/main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="publish" [isFreeName]
    v2 = Data data=d0 @10
    v3 = Call v1, -, v2
    v4 = Data data=d1 @20
    v5 = Call v1, -, v4
    v6 = Data data=d2 @30
    v7 = Call v1, -, v6
    v8 = CallDirect v0 function=f1 [isNeverInlined]
    v9 = Call v1, -, v8
    v10 = Data data=d4 @50
    v11 = Call v1, -, v10
    ElemSet v10, v11, v11 mode=Strict
    v13 = Data data=d5 @60
    v14 = Call v1, -, v13
    ElemSet v13, v14, v14 mode=Strict
    v16 = Data data=d6 @70
    v17 = Call v1, -, v16
    v18 = CallDirect v0 function=f1 [isNeverInlined]
    Return
}
function f1 name="runsTwice" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = Data data=d3 @40
    Return v1
}
data d0 root=Object{layout=L0 members=[{key="mixed" value=Array{contents=anything elements=[Number{value=1}, Hole, String{value="pear"}, Boolean{value=true}, Undefined, Null, Boolean{}]}}, {key="doubles" value=Array{contents=number elements=[Number{value=1.5}, Hole, Number{value=2}, Number{value=-0}]}}, {key="integers" value=Array{contents=number elements=[Number{value=1}, Number{value=2}]}}, {key="inner" value=Object{layout=L1 members=[{key="zero" value=Number{value=-0}}, {key="nan" value=Number{value=NaN}}, {key="nothing" value=Null}]}}, {key="big" value=BigInt{digits="12345678901234567890"}}, {key="text" value=String{value="plum"}}, {key="nested" value=Array{contents=anything elements=[Array{contents=anything elements=[Object{layout=L1 members=[{key="zero" value=Number{value=7}}, {key="nan" value=Number{value=8}}, {key="nothing" value=Undefined}]}]}, Array{contents=anything}]}}]} module=m0
data d1 root=Array{contents=i32 elements=[Number{value=1}, Number{value=2}, Number{value=3}, Number{value=4}, Number{value=5}, Number{value=6}, Number{value=7}, Number{value=8}, Number{value=9}, Number{value=10}, Number{value=11}, Number{value=12}, Number{value=13}, Number{value=14}, Number{value=15}, Number{value=16}, Number{value=17}]} module=m0
data d2 root=Array{contents=number elements=[Number{value=0.5}, Number{value=2}, Number{value=3}, Number{value=4}, Number{value=5}, Number{value=6}, Number{value=7}, Number{value=8}, Number{value=9}, Number{value=10}, Number{value=11}, Number{value=12}, Number{value=13}, Number{value=14}, Number{value=15}, Number{value=16}, Number{value=-0}]} module=m0
data d3 root=Array{contents=anything elements=[String{value="a0"}, Number{value=2}, Undefined, Null, Boolean{value=true}, String{value="a5"}, Number{value=7}, Number{value=8}, Number{value=9}, Number{value=10}, Number{value=11}, Number{value=12}, Number{value=13}, Number{value=14}, Number{value=15}, Number{value=16}, String{value="a16"}]} module=m0
data d4 root=Object{layout=L3 members=[{key="name" value=String{value="fig"}}, {key="10" value=String{value="ten"}}, {key="size" value=Number{value=3}}, {key="content-type" value=String{value="text"}}, {key="2" value=Number{value=-0}}, {key="nested" value=Object{layout=L2 members=[{key="deep" value=Array{contents=anything elements=[Object{layout=L2 members=[{key="deeper" value=BigInt{digits="12345678901234567890"}}]}]}}, {key="constructor" value=Null}]}}, {key="empty" value=Object{layout=L2}}, {key="w0" value=Number{value=0}}, {key="w1" value=Number{value=1}}, {key="w2" value=Number{value=2}}, {key="w3" value=Number{value=3}}, {key="w4" value=Number{value=4}}, {key="w5" value=Number{value=5}}, {key="w6" value=Number{value=6}}, {key="w7" value=Number{value=7}}, {key="w8" value=Number{value=8}}, {key="w9" value=Number{value=9}}, {key="w10" value=Number{value=10}}, {key="w11" value=Number{value=11}}]} module=m0
data d5 root=Object{layout=L2 members=[{key="key 0" value=Number{value=0}}, {key="key 1" value=String{value="v1"}}, {key="key 2" value=Number{value=2}}, {key="key 3" value=String{value="v3"}}, {key="key 4" value=Number{value=4}}, {key="key 5" value=String{value="v5"}}, {key="key 6" value=Number{value=6}}, {key="key 7" value=String{value="v7"}}, {key="key 8" value=Number{value=8}}, {key="key 9" value=String{value="v9"}}, {key="key 10" value=Number{value=10}}, {key="key 11" value=String{value="v11"}}, {key="key 12" value=Number{value=12}}, {key="key 13" value=String{value="v13"}}, {key="key 14" value=Number{value=14}}, {key="key 15" value=String{value="v15"}}, {key="key 16" value=Number{value=16}}, {key="key 17" value=String{value="v17"}}, {key="key 18" value=Number{value=18}}, {key="key 19" value=String{value="v19"}}, {key="key 20" value=Number{value=20}}, {key="key 21" value=String{value="v21"}}, {key="key 22" value=Number{value=22}}, {key="key 23" value=String{value="v23"}}, {key="key 24" value=Number{value=24}}, {key="key 25" value=String{value="v25"}}, {key="key 26" value=Number{value=26}}, {key="key 27" value=String{value="v27"}}, {key="key 28" value=Number{value=28}}, {key="key 29" value=String{value="v29"}}, {key="key 30" value=Number{value=30}}, {key="key 31" value=String{value="v31"}}, {key="key 32" value=Number{value=32}}, {key="key 33" value=String{value="v33"}}, {key="key 34" value=Number{value=34}}, {key="key 35" value=String{value="v35"}}, {key="key 36" value=Number{value=36}}, {key="key 37" value=String{value="v37"}}, {key="key 38" value=Number{value=38}}, {key="key 39" value=String{value="v39"}}, {key="key 40" value=Number{value=40}}, {key="key 41" value=String{value="v41"}}, {key="key 42" value=Number{value=42}}, {key="key 43" value=String{value="v43"}}, {key="key 44" value=Number{value=44}}, {key="key 45" value=String{value="v45"}}, {key="key 46" value=Number{value=46}}, {key="key 47" value=String{value="v47"}}, {key="key 48" value=Number{value=48}}, {key="key 49" value=String{value="v49"}}, {key="key 50" value=Number{value=50}}, {key="key 51" value=String{value="v51"}}, {key="key 52" value=Number{value=52}}, {key="key 53" value=String{value="v53"}}, {key="key 54" value=Number{value=54}}, {key="key 55" value=String{value="v55"}}, {key="key 56" value=Number{value=56}}, {key="key 57" value=String{value="v57"}}, {key="key 58" value=Number{value=58}}, {key="key 59" value=String{value="v59"}}, {key="key 60" value=Number{value=60}}, {key="key 61" value=String{value="v61"}}, {key="key 62" value=Number{value=62}}, {key="key 63" value=String{value="v63"}}, {key="key 64" value=Number{value=64}}, {key="key 65" value=String{value="v65"}}, {key="key 66" value=Number{value=66}}, {key="key 67" value=String{value="v67"}}, {key="key 68" value=Number{value=68}}, {key="key 69" value=String{value="v69"}}, {key="key 70" value=Number{value=70}}, {key="key 71" value=String{value="v71"}}, {key="key 72" value=Number{value=72}}, {key="key 73" value=String{value="v73"}}, {key="key 74" value=Number{value=74}}, {key="key 75" value=String{value="v75"}}, {key="key 76" value=Number{value=76}}, {key="key 77" value=String{value="v77"}}, {key="key 78" value=Number{value=78}}, {key="key 79" value=String{value="v79"}}, {key="key 80" value=Number{value=80}}, {key="key 81" value=String{value="v81"}}, {key="key 82" value=Number{value=82}}, {key="key 83" value=String{value="v83"}}, {key="key 84" value=Number{value=84}}, {key="key 85" value=String{value="v85"}}, {key="key 86" value=Number{value=86}}, {key="key 87" value=String{value="v87"}}, {key="key 88" value=Number{value=88}}, {key="key 89" value=String{value="v89"}}, {key="key 90" value=Number{value=90}}, {key="key 91" value=String{value="v91"}}, {key="key 92" value=Number{value=92}}, {key="key 93" value=String{value="v93"}}, {key="key 94" value=Number{value=94}}, {key="key 95" value=String{value="v95"}}, {key="key 96" value=Number{value=96}}, {key="key 97" value=String{value="v97"}}, {key="key 98" value=Number{value=98}}, {key="key 99" value=String{value="v99"}}, {key="key 100" value=Number{value=100}}, {key="key 101" value=String{value="v101"}}, {key="key 102" value=Number{value=102}}, {key="key 103" value=String{value="v103"}}, {key="key 104" value=Number{value=104}}, {key="key 105" value=String{value="v105"}}, {key="key 106" value=Number{value=106}}, {key="key 107" value=String{value="v107"}}, {key="key 108" value=Number{value=108}}, {key="key 109" value=String{value="v109"}}, {key="key 110" value=Number{value=110}}, {key="key 111" value=String{value="v111"}}, {key="key 112" value=Number{value=112}}, {key="key 113" value=String{value="v113"}}, {key="key 114" value=Number{value=114}}, {key="key 115" value=String{value="v115"}}, {key="key 116" value=Number{value=116}}, {key="key 117" value=String{value="v117"}}, {key="key 118" value=Number{value=118}}, {key="key 119" value=String{value="v119"}}, {key="key 120" value=Number{value=120}}, {key="key 121" value=String{value="v121"}}, {key="key 122" value=Number{value=122}}, {key="key 123" value=String{value="v123"}}, {key="key 124" value=Number{value=124}}, {key="key 125" value=String{value="v125"}}, {key="key 126" value=Number{value=126}}, {key="key 127" value=String{value="v127"}}, {key="key 128" value=Number{value=128}}, {key="key 129" value=String{value="v129"}}, {key="key 130" value=Number{value=130}}, {key="key 131" value=String{value="v131"}}, {key="key 132" value=Number{value=132}}, {key="key 133" value=String{value="v133"}}, {key="key 134" value=Number{value=134}}, {key="key 135" value=String{value="v135"}}, {key="key 136" value=Number{value=136}}, {key="key 137" value=String{value="v137"}}, {key="key 138" value=Number{value=138}}, {key="key 139" value=String{value="v139"}}, {key="key 140" value=Number{value=140}}, {key="key 141" value=String{value="v141"}}, {key="key 142" value=Number{value=142}}, {key="key 143" value=String{value="v143"}}, {key="key 144" value=Number{value=144}}, {key="key 145" value=String{value="v145"}}, {key="key 146" value=Number{value=146}}, {key="key 147" value=String{value="v147"}}, {key="key 148" value=Number{value=148}}, {key="key 149" value=String{value="v149"}}, {key="key 150" value=Number{value=150}}, {key="key 151" value=String{value="v151"}}, {key="key 152" value=Number{value=152}}, {key="key 153" value=String{value="v153"}}, {key="key 154" value=Number{value=154}}, {key="key 155" value=String{value="v155"}}, {key="key 156" value=Number{value=156}}, {key="key 157" value=String{value="v157"}}, {key="key 158" value=Number{value=158}}, {key="key 159" value=String{value="v159"}}, {key="key 160" value=Number{value=160}}, {key="key 161" value=String{value="v161"}}, {key="key 162" value=Number{value=162}}, {key="key 163" value=String{value="v163"}}, {key="key 164" value=Number{value=164}}, {key="key 165" value=String{value="v165"}}, {key="key 166" value=Number{value=166}}, {key="key 167" value=String{value="v167"}}, {key="key 168" value=Number{value=168}}, {key="key 169" value=String{value="v169"}}, {key="key 170" value=Number{value=170}}, {key="key 171" value=String{value="v171"}}, {key="key 172" value=Number{value=172}}, {key="key 173" value=String{value="v173"}}, {key="key 174" value=Number{value=174}}, {key="key 175" value=String{value="v175"}}, {key="key 176" value=Number{value=176}}, {key="key 177" value=String{value="v177"}}, {key="key 178" value=Number{value=178}}, {key="key 179" value=String{value="v179"}}, {key="key 180" value=Number{value=180}}, {key="key 181" value=String{value="v181"}}, {key="key 182" value=Number{value=182}}, {key="key 183" value=String{value="v183"}}, {key="key 184" value=Number{value=184}}, {key="key 185" value=String{value="v185"}}, {key="key 186" value=Number{value=186}}, {key="key 187" value=String{value="v187"}}, {key="key 188" value=Number{value=188}}, {key="key 189" value=String{value="v189"}}, {key="key 190" value=Number{value=190}}, {key="key 191" value=String{value="v191"}}, {key="key 192" value=Number{value=192}}, {key="key 193" value=String{value="v193"}}, {key="key 194" value=Number{value=194}}, {key="key 195" value=String{value="v195"}}, {key="key 196" value=Number{value=196}}, {key="key 197" value=String{value="v197"}}, {key="key 198" value=Number{value=198}}, {key="key 199" value=String{value="v199"}}, {key="key 200" value=Number{value=200}}, {key="key 201" value=String{value="v201"}}, {key="key 202" value=Number{value=202}}, {key="key 203" value=String{value="v203"}}, {key="key 204" value=Number{value=204}}, {key="key 205" value=String{value="v205"}}, {key="key 206" value=Number{value=206}}, {key="key 207" value=String{value="v207"}}, {key="key 208" value=Number{value=208}}, {key="key 209" value=String{value="v209"}}, {key="key 210" value=Number{value=210}}, {key="key 211" value=String{value="v211"}}, {key="key 212" value=Number{value=212}}, {key="key 213" value=String{value="v213"}}, {key="key 214" value=Number{value=214}}, {key="key 215" value=String{value="v215"}}, {key="key 216" value=Number{value=216}}, {key="key 217" value=String{value="v217"}}, {key="key 218" value=Number{value=218}}, {key="key 219" value=String{value="v219"}}, {key="key 220" value=Number{value=220}}, {key="key 221" value=String{value="v221"}}, {key="key 222" value=Number{value=222}}, {key="key 223" value=String{value="v223"}}, {key="key 224" value=Number{value=224}}, {key="key 225" value=String{value="v225"}}, {key="key 226" value=Number{value=226}}, {key="key 227" value=String{value="v227"}}, {key="key 228" value=Number{value=228}}, {key="key 229" value=String{value="v229"}}, {key="key 230" value=Number{value=230}}, {key="key 231" value=String{value="v231"}}, {key="key 232" value=Number{value=232}}, {key="key 233" value=String{value="v233"}}, {key="key 234" value=Number{value=234}}, {key="key 235" value=String{value="v235"}}, {key="key 236" value=Number{value=236}}, {key="key 237" value=String{value="v237"}}, {key="key 238" value=Number{value=238}}, {key="key 239" value=String{value="v239"}}, {key="key 240" value=Number{value=240}}, {key="key 241" value=String{value="v241"}}, {key="key 242" value=Number{value=242}}, {key="key 243" value=String{value="v243"}}, {key="key 244" value=Number{value=244}}, {key="key 245" value=String{value="v245"}}, {key="key 246" value=Number{value=246}}, {key="key 247" value=String{value="v247"}}, {key="key 248" value=Number{value=248}}, {key="key 249" value=String{value="v249"}}, {key="key 250" value=Number{value=250}}, {key="key 251" value=String{value="v251"}}, {key="key 252" value=Number{value=252}}, {key="key 253" value=String{value="v253"}}, {key="key 254" value=Number{value=254}}, {key="key 255" value=String{value="v255"}}, {key="key 256" value=Number{value=256}}, {key="key 257" value=String{value="v257"}}, {key="key 258" value=Number{value=258}}, {key="key 259" value=String{value="v259"}}, {key="key 260" value=Number{value=260}}, {key="key 261" value=String{value="v261"}}, {key="key 262" value=Number{value=262}}, {key="key 263" value=String{value="v263"}}, {key="key 264" value=Number{value=264}}, {key="key 265" value=String{value="v265"}}, {key="key 266" value=Number{value=266}}, {key="key 267" value=String{value="v267"}}, {key="key 268" value=Number{value=268}}, {key="key 269" value=String{value="v269"}}, {key="key 270" value=Number{value=270}}, {key="key 271" value=String{value="v271"}}, {key="key 272" value=Number{value=272}}, {key="key 273" value=String{value="v273"}}, {key="key 274" value=Number{value=274}}, {key="key 275" value=String{value="v275"}}, {key="key 276" value=Number{value=276}}, {key="key 277" value=String{value="v277"}}, {key="key 278" value=Number{value=278}}, {key="key 279" value=String{value="v279"}}, {key="key 280" value=Number{value=280}}, {key="key 281" value=String{value="v281"}}, {key="key 282" value=Number{value=282}}, {key="key 283" value=String{value="v283"}}, {key="key 284" value=Number{value=284}}, {key="key 285" value=String{value="v285"}}, {key="key 286" value=Number{value=286}}, {key="key 287" value=String{value="v287"}}, {key="key 288" value=Number{value=288}}, {key="key 289" value=String{value="v289"}}, {key="key 290" value=Number{value=290}}, {key="key 291" value=String{value="v291"}}, {key="key 292" value=Number{value=292}}, {key="key 293" value=String{value="v293"}}, {key="key 294" value=Number{value=294}}, {key="key 295" value=String{value="v295"}}, {key="key 296" value=Number{value=296}}, {key="key 297" value=String{value="v297"}}, {key="key 298" value=Number{value=298}}, {key="key 299" value=String{value="v299"}}, {key="key 300" value=Number{value=300}}, {key="key 301" value=String{value="v301"}}, {key="key 302" value=Number{value=302}}, {key="key 303" value=String{value="v303"}}, {key="key 304" value=Number{value=304}}, {key="key 305" value=String{value="v305"}}, {key="key 306" value=Number{value=306}}, {key="key 307" value=String{value="v307"}}, {key="key 308" value=Number{value=308}}, {key="key 309" value=String{value="v309"}}, {key="key 310" value=Number{value=310}}, {key="key 311" value=String{value="v311"}}, {key="key 312" value=Number{value=312}}, {key="key 313" value=String{value="v313"}}, {key="key 314" value=Number{value=314}}, {key="key 315" value=String{value="v315"}}, {key="key 316" value=Number{value=316}}, {key="key 317" value=String{value="v317"}}, {key="key 318" value=Number{value=318}}, {key="key 319" value=String{value="v319"}}, {key="key 320" value=Number{value=320}}, {key="key 321" value=String{value="v321"}}, {key="key 322" value=Number{value=322}}, {key="key 323" value=String{value="v323"}}, {key="key 324" value=Number{value=324}}, {key="key 325" value=String{value="v325"}}, {key="key 326" value=Number{value=326}}, {key="key 327" value=String{value="v327"}}, {key="key 328" value=Number{value=328}}, {key="key 329" value=String{value="v329"}}, {key="key 330" value=Number{value=330}}, {key="key 331" value=String{value="v331"}}, {key="key 332" value=Number{value=332}}, {key="key 333" value=String{value="v333"}}, {key="key 334" value=Number{value=334}}, {key="key 335" value=String{value="v335"}}, {key="key 336" value=Number{value=336}}, {key="key 337" value=String{value="v337"}}, {key="key 338" value=Number{value=338}}, {key="key 339" value=String{value="v339"}}, {key="key 340" value=Number{value=340}}, {key="key 341" value=String{value="v341"}}, {key="key 342" value=Number{value=342}}, {key="key 343" value=String{value="v343"}}, {key="key 344" value=Number{value=344}}, {key="key 345" value=String{value="v345"}}, {key="key 346" value=Number{value=346}}, {key="key 347" value=String{value="v347"}}, {key="key 348" value=Number{value=348}}, {key="key 349" value=String{value="v349"}}, {key="key 350" value=Number{value=350}}, {key="key 351" value=String{value="v351"}}, {key="key 352" value=Number{value=352}}, {key="key 353" value=String{value="v353"}}, {key="key 354" value=Number{value=354}}, {key="key 355" value=String{value="v355"}}, {key="key 356" value=Number{value=356}}, {key="key 357" value=String{value="v357"}}, {key="key 358" value=Number{value=358}}, {key="key 359" value=String{value="v359"}}, {key="key 360" value=Number{value=360}}, {key="key 361" value=String{value="v361"}}, {key="key 362" value=Number{value=362}}, {key="key 363" value=String{value="v363"}}, {key="key 364" value=Number{value=364}}, {key="key 365" value=String{value="v365"}}, {key="key 366" value=Number{value=366}}, {key="key 367" value=String{value="v367"}}, {key="key 368" value=Number{value=368}}, {key="key 369" value=String{value="v369"}}, {key="key 370" value=Number{value=370}}, {key="key 371" value=String{value="v371"}}, {key="key 372" value=Number{value=372}}, {key="key 373" value=String{value="v373"}}, {key="key 374" value=Number{value=374}}, {key="key 375" value=String{value="v375"}}, {key="key 376" value=Number{value=376}}, {key="key 377" value=String{value="v377"}}, {key="key 378" value=Number{value=378}}, {key="key 379" value=String{value="v379"}}, {key="key 380" value=Number{value=380}}, {key="key 381" value=String{value="v381"}}, {key="key 382" value=Number{value=382}}, {key="key 383" value=String{value="v383"}}, {key="key 384" value=Number{value=384}}, {key="key 385" value=String{value="v385"}}, {key="key 386" value=Number{value=386}}, {key="key 387" value=String{value="v387"}}, {key="key 388" value=Number{value=388}}, {key="key 389" value=String{value="v389"}}, {key="key 390" value=Number{value=390}}, {key="key 391" value=String{value="v391"}}, {key="key 392" value=Number{value=392}}, {key="key 393" value=String{value="v393"}}, {key="key 394" value=Number{value=394}}, {key="key 395" value=String{value="v395"}}, {key="key 396" value=Number{value=396}}, {key="key 397" value=String{value="v397"}}, {key="key 398" value=Number{value=398}}, {key="key 399" value=String{value="v399"}}, {key="key 400" value=Number{value=400}}, {key="key 401" value=String{value="v401"}}, {key="key 402" value=Number{value=402}}, {key="key 403" value=String{value="v403"}}, {key="key 404" value=Number{value=404}}, {key="key 405" value=String{value="v405"}}, {key="key 406" value=Number{value=406}}, {key="key 407" value=String{value="v407"}}, {key="key 408" value=Number{value=408}}, {key="key 409" value=String{value="v409"}}, {key="key 410" value=Number{value=410}}, {key="key 411" value=String{value="v411"}}, {key="key 412" value=Number{value=412}}, {key="key 413" value=String{value="v413"}}, {key="key 414" value=Number{value=414}}, {key="key 415" value=String{value="v415"}}, {key="key 416" value=Number{value=416}}, {key="key 417" value=String{value="v417"}}, {key="key 418" value=Number{value=418}}, {key="key 419" value=String{value="v419"}}, {key="key 420" value=Number{value=420}}, {key="key 421" value=String{value="v421"}}, {key="key 422" value=Number{value=422}}, {key="key 423" value=String{value="v423"}}, {key="key 424" value=Number{value=424}}, {key="key 425" value=String{value="v425"}}, {key="key 426" value=Number{value=426}}, {key="key 427" value=String{value="v427"}}, {key="key 428" value=Number{value=428}}, {key="key 429" value=String{value="v429"}}, {key="key 430" value=Number{value=430}}, {key="key 431" value=String{value="v431"}}, {key="key 432" value=Number{value=432}}, {key="key 433" value=String{value="v433"}}, {key="key 434" value=Number{value=434}}, {key="key 435" value=String{value="v435"}}, {key="key 436" value=Number{value=436}}, {key="key 437" value=String{value="v437"}}, {key="key 438" value=Number{value=438}}, {key="key 439" value=String{value="v439"}}, {key="key 440" value=Number{value=440}}, {key="key 441" value=String{value="v441"}}, {key="key 442" value=Number{value=442}}, {key="key 443" value=String{value="v443"}}, {key="key 444" value=Number{value=444}}, {key="key 445" value=String{value="v445"}}, {key="key 446" value=Number{value=446}}, {key="key 447" value=String{value="v447"}}, {key="key 448" value=Number{value=448}}, {key="key 449" value=String{value="v449"}}, {key="key 450" value=Number{value=450}}, {key="key 451" value=String{value="v451"}}, {key="key 452" value=Number{value=452}}, {key="key 453" value=String{value="v453"}}, {key="key 454" value=Number{value=454}}, {key="key 455" value=String{value="v455"}}, {key="key 456" value=Number{value=456}}, {key="key 457" value=String{value="v457"}}, {key="key 458" value=Number{value=458}}, {key="key 459" value=String{value="v459"}}, {key="key 460" value=Number{value=460}}, {key="key 461" value=String{value="v461"}}, {key="key 462" value=Number{value=462}}, {key="key 463" value=String{value="v463"}}, {key="key 464" value=Number{value=464}}, {key="key 465" value=String{value="v465"}}, {key="key 466" value=Number{value=466}}, {key="key 467" value=String{value="v467"}}, {key="key 468" value=Number{value=468}}, {key="key 469" value=String{value="v469"}}, {key="key 470" value=Number{value=470}}, {key="key 471" value=String{value="v471"}}, {key="key 472" value=Number{value=472}}, {key="key 473" value=String{value="v473"}}, {key="key 474" value=Number{value=474}}, {key="key 475" value=String{value="v475"}}, {key="key 476" value=Number{value=476}}, {key="key 477" value=String{value="v477"}}, {key="key 478" value=Number{value=478}}, {key="key 479" value=String{value="v479"}}, {key="key 480" value=Number{value=480}}, {key="key 481" value=String{value="v481"}}, {key="key 482" value=Number{value=482}}, {key="key 483" value=String{value="v483"}}, {key="key 484" value=Number{value=484}}, {key="key 485" value=String{value="v485"}}, {key="key 486" value=Number{value=486}}, {key="key 487" value=String{value="v487"}}, {key="key 488" value=Number{value=488}}, {key="key 489" value=String{value="v489"}}, {key="key 490" value=Number{value=490}}, {key="key 491" value=String{value="v491"}}, {key="key 492" value=Number{value=492}}, {key="key 493" value=String{value="v493"}}, {key="key 494" value=Number{value=494}}, {key="key 495" value=String{value="v495"}}, {key="key 496" value=Number{value=496}}, {key="key 497" value=String{value="v497"}}, {key="key 498" value=Number{value=498}}, {key="key 499" value=String{value="v499"}}, {key="key 500" value=Number{value=500}}, {key="key 501" value=String{value="v501"}}, {key="key 502" value=Number{value=502}}, {key="key 503" value=String{value="v503"}}, {key="key 504" value=Number{value=504}}, {key="key 505" value=String{value="v505"}}, {key="key 506" value=Number{value=506}}, {key="key 507" value=String{value="v507"}}, {key="key 508" value=Number{value=508}}, {key="key 509" value=String{value="v509"}}, {key="key 510" value=Number{value=510}}, {key="key 511" value=String{value="v511"}}, {key="key 512" value=Number{value=512}}, {key="key 513" value=String{value="v513"}}]} module=m0
data d6 root=Object{layout=L4 members=[{key="first" value=Number{value=1}}, {key="3" value=String{value="three"}}, {key="m0" value=Number{value=0}}, {key="m1" value=Number{value=1}}, {key="m2" value=Number{value=2}}, {key="m3" value=Number{value=3}}, {key="m4" value=Number{value=4}}, {key="m5" value=Number{value=5}}, {key="m6" value=Number{value=6}}, {key="m7" value=Number{value=7}}, {key="m8" value=Number{value=8}}, {key="m9" value=Number{value=9}}, {key="m10" value=Number{value=10}}, {key="m11" value=Number{value=11}}, {key="m12" value=Number{value=12}}, {key="m13" value=Number{value=13}}, {key="m14" value=Number{value=14}}, {key="m15" value=Number{value=15}}]} module=m0
)IR"_s;

// const few = { one: 1, "content-type": "two", nothing: null }, many = { "name 0": 1, "name 1": "two", .. }, each with a type that names no keys.
static constexpr ASCIILiteral programWithDictionaryLiteralsText = R"IR(module m0 path="/main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="publish" [isFreeName]
    v2 = Integer integer=1
    v3 = String string="two"
    v4 = Null
    v5 = NewDictionary @10
    PropDefine v5, v2 key="one"
    PropDefine v5, v3 key="content-type"
    PropDefine v5, v4 key="nothing"
    v9 = Call v1, -, v5
    v10 = NewDictionary @30
    PropDefine v10, v2 key="name 0"
    PropDefine v10, v3 key="name 1"
    PropDefine v10, v4 key="name 2"
    PropDefine v10, v2 key="name 3"
    PropDefine v10, v3 key="name 4"
    PropDefine v10, v4 key="name 5"
    PropDefine v10, v2 key="name 6"
    PropDefine v10, v3 key="name 7"
    PropDefine v10, v4 key="name 8"
    PropDefine v10, v2 key="name 9"
    PropDefine v10, v3 key="name 10"
    PropDefine v10, v4 key="name 11"
    PropDefine v10, v2 key="name 12"
    PropDefine v10, v3 key="name 13"
    PropDefine v10, v4 key="name 14"
    PropDefine v10, v2 key="name 15"
    PropDefine v10, v3 key="name 16"
    PropDefine v10, v4 key="name 17"
    PropDefine v10, v2 key="name 18"
    PropDefine v10, v3 key="name 19"
    PropDefine v10, v4 key="name 20"
    PropDefine v10, v2 key="name 21"
    PropDefine v10, v3 key="name 22"
    PropDefine v10, v4 key="name 23"
    PropDefine v10, v2 key="name 24"
    PropDefine v10, v3 key="name 25"
    PropDefine v10, v4 key="name 26"
    PropDefine v10, v2 key="name 27"
    PropDefine v10, v3 key="name 28"
    PropDefine v10, v4 key="name 29"
    PropDefine v10, v2 key="name 30"
    PropDefine v10, v3 key="name 31"
    PropDefine v10, v4 key="name 32"
    PropDefine v10, v2 key="name 33"
    PropDefine v10, v3 key="name 34"
    PropDefine v10, v4 key="name 35"
    PropDefine v10, v2 key="name 36"
    PropDefine v10, v3 key="name 37"
    PropDefine v10, v4 key="name 38"
    PropDefine v10, v2 key="name 39"
    PropDefine v10, v3 key="name 40"
    PropDefine v10, v4 key="name 41"
    PropDefine v10, v2 key="name 42"
    PropDefine v10, v3 key="name 43"
    PropDefine v10, v4 key="name 44"
    PropDefine v10, v2 key="name 45"
    PropDefine v10, v3 key="name 46"
    PropDefine v10, v4 key="name 47"
    PropDefine v10, v2 key="name 48"
    PropDefine v10, v3 key="name 49"
    PropDefine v10, v4 key="name 50"
    PropDefine v10, v2 key="name 51"
    PropDefine v10, v3 key="name 52"
    PropDefine v10, v4 key="name 53"
    PropDefine v10, v2 key="name 54"
    PropDefine v10, v3 key="name 55"
    PropDefine v10, v4 key="name 56"
    PropDefine v10, v2 key="name 57"
    PropDefine v10, v3 key="name 58"
    PropDefine v10, v4 key="name 59"
    PropDefine v10, v2 key="name 60"
    PropDefine v10, v3 key="name 61"
    PropDefine v10, v4 key="name 62"
    PropDefine v10, v2 key="name 63"
    PropDefine v10, v3 key="name 64"
    PropDefine v10, v4 key="name 65"
    PropDefine v10, v2 key="name 66"
    PropDefine v10, v3 key="name 67"
    PropDefine v10, v4 key="name 68"
    PropDefine v10, v2 key="name 69"
    PropDefine v10, v3 key="name 70"
    PropDefine v10, v4 key="name 71"
    PropDefine v10, v2 key="name 72"
    PropDefine v10, v3 key="name 73"
    PropDefine v10, v4 key="name 74"
    PropDefine v10, v2 key="name 75"
    PropDefine v10, v3 key="name 76"
    PropDefine v10, v4 key="name 77"
    PropDefine v10, v2 key="name 78"
    PropDefine v10, v3 key="name 79"
    PropDefine v10, v4 key="name 80"
    PropDefine v10, v2 key="name 81"
    PropDefine v10, v3 key="name 82"
    PropDefine v10, v4 key="name 83"
    PropDefine v10, v2 key="name 84"
    PropDefine v10, v3 key="name 85"
    PropDefine v10, v4 key="name 86"
    PropDefine v10, v2 key="name 87"
    PropDefine v10, v3 key="name 88"
    PropDefine v10, v4 key="name 89"
    PropDefine v10, v2 key="name 90"
    PropDefine v10, v3 key="name 91"
    PropDefine v10, v4 key="name 92"
    PropDefine v10, v2 key="name 93"
    PropDefine v10, v3 key="name 94"
    PropDefine v10, v4 key="name 95"
    PropDefine v10, v2 key="name 96"
    PropDefine v10, v3 key="name 97"
    PropDefine v10, v4 key="name 98"
    PropDefine v10, v2 key="name 99"
    PropDefine v10, v3 key="name 100"
    PropDefine v10, v4 key="name 101"
    PropDefine v10, v2 key="name 102"
    PropDefine v10, v3 key="name 103"
    PropDefine v10, v4 key="name 104"
    PropDefine v10, v2 key="name 105"
    PropDefine v10, v3 key="name 106"
    PropDefine v10, v4 key="name 107"
    PropDefine v10, v2 key="name 108"
    PropDefine v10, v3 key="name 109"
    PropDefine v10, v4 key="name 110"
    PropDefine v10, v2 key="name 111"
    PropDefine v10, v3 key="name 112"
    PropDefine v10, v4 key="name 113"
    PropDefine v10, v2 key="name 114"
    PropDefine v10, v3 key="name 115"
    PropDefine v10, v4 key="name 116"
    PropDefine v10, v2 key="name 117"
    PropDefine v10, v3 key="name 118"
    PropDefine v10, v4 key="name 119"
    PropDefine v10, v2 key="name 120"
    PropDefine v10, v3 key="name 121"
    PropDefine v10, v4 key="name 122"
    PropDefine v10, v2 key="name 123"
    PropDefine v10, v3 key="name 124"
    PropDefine v10, v4 key="name 125"
    PropDefine v10, v2 key="name 126"
    PropDefine v10, v3 key="name 127"
    PropDefine v10, v4 key="name 128"
    PropDefine v10, v2 key="name 129"
    v141 = Call v1, -, v10
    Return
}
)IR"_s;
static constexpr unsigned numberOfNamesInLargeLiteral = 130;
static_assert(numberOfNamesInLargeLiteral > static_cast<unsigned>(Structure::s_maxTransitionLength));

static constexpr ASCIILiteral describedTree = "{mixed:[1,hole,\"pear\",true,undefined,null,false],doubles:[1.5,hole,2,-0],integers:[1,2],inner:{zero:-0,nan:NaN,nothing:null},big:12345678901234567890n,text:\"plum\",nested:[[{zero:7,nan:8,nothing:undefined}],[]]}"_s;
static constexpr ASCIILiteral describedIntegers = "[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17]"_s;
static constexpr ASCIILiteral describedDoubles = "[0.5,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,-0]"_s;
static constexpr ASCIILiteral describedMixture = "[\"a0\",2,undefined,null,true,\"a5\",7,8,9,10,11,12,13,14,15,16,\"a16\"]"_s;

static constexpr ASCIILiteral describedDictionary = "{2:-0,10:\"ten\",name:\"fig\",size:3,content-type:\"text\",nested:{deep:[{deeper:12345678901234567890n}],constructor:null},empty:{},w0:0,w1:1,w2:2,w3:3,w4:4,w5:5,w6:6,w7:7,w8:8,w9:9,w10:10,w11:11}"_s;
static constexpr ASCIILiteral describedOrdinaryObject = "{3:\"three\",first:1,m0:0,m1:1,m2:2,m3:3,m4:4,m5:5,m6:6,m7:7,m8:8,m9:9,m10:10,m11:11,m12:12,m13:13,m14:14,m15:15}"_s;
// More than Structure::createWithProperties() puts into one Structure that every such object shares.
static constexpr unsigned numberOfKeysInLargeDictionary = 514;
static_assert(numberOfKeysInLargeDictionary > static_cast<unsigned>(Structure::s_maxTransitionLengthForNonEvalPutById));

static constexpr ASCIILiteral describeSource = R"js((function describe(value) {
    if (Array.isArray(value)) {
        let parts = [];
        for (let i = 0; i < value.length; ++i)
            parts.push(i in value ? describe(value[i]) : "hole");
        return "[" + parts.join(",") + "]";
    }
    if (typeof value === "object" && value !== null)
        return "{" + Object.keys(value).map(key => key + ":" + describe(value[key])).join(",") + "}";
    if (typeof value === "string")
        return JSON.stringify(value);
    if (typeof value === "bigint")
        return value + "n";
    return Object.is(value, -0) ? "-0" : String(value);
}))js"_s;

enum DataNumber : unsigned { Tree, Integers, Doubles, Dictionary, LargeDictionary, SeenByUnknownCodeOnly, Mixture, NumberOfData };

static void testDataWordsRoundTrip()
{
    const JSValue immediates[] = {
        jsUndefined(), jsNull(), jsBoolean(true), jsBoolean(false),
        jsNumber(0), jsNumber(-1), jsNumber(INT32_MAX), jsNumber(INT32_MIN),
        jsDoubleNumber(1.5), jsDoubleNumber(-0.0), jsDoubleNumber(PNaN), jsDoubleNumber(std::numeric_limits<double>::infinity()), jsDoubleNumber(5e-324),
    };
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> words;
    AOT_TEST_CHECK(DataWord::kindOf(DataWord::hole) == DataWord::Kind::Immediate);
    AOT_TEST_CHECK(DataWord::encodeImmediate(JSValue()) == DataWord::hole);
    AOT_TEST_CHECK(words.add(DataWord::hole).isNewEntry);
    for (JSValue immediate : immediates) {
        uint64_t word = DataWord::encodeImmediate(immediate);
        AOT_TEST_CHECK(DataWord::kindOf(word) == DataWord::Kind::Immediate);
        AOT_TEST_CHECK(JSValue::encode(JSValue::decode(static_cast<EncodedJSValue>(word))) == JSValue::encode(immediate));
        AOT_TEST_CHECK(words.add(word).isNewEntry);
    }

    for (uint32_t operand : { 0u, 1u, 7u, 8u, 65535u, 1u << 24, UINT32_MAX - 1 }) {
        struct Case {
            uint64_t word;
            DataWord::Kind kind;
        };
        const Case cases[] = {
            { DataWord::encodeStringConstant(operand), DataWord::Kind::StringConstant },
            { DataWord::encodeBigIntConstant(operand), DataWord::Kind::BigIntConstant },
            { DataWord::encodeNode(operand), DataWord::Kind::Node },
        };
        for (const Case& testCase : cases) {
            AOT_TEST_CHECK(DataWord::kindOf(testCase.word) == testCase.kind);
            AOT_TEST_CHECK(DataWord::operandOf(testCase.word) == operand);
            AOT_TEST_CHECK(words.add(testCase.word).isNewEntry);
        }
    }
}

static void testNumberOfWords()
{
    using Kind = DataNodeHeader::Kind;
    struct Case {
        uint32_t count;
        uint32_t wordsOfArray;
        uint32_t wordsOfObject;
    };
    static constexpr Case cases[] = {
        { 0, 2, 1 }, { 1, 3, 3 }, { 3, 5, 5 }, { 4, 6, 6 }, { 5, 7, 8 }, { 8, 10, 11 }, { 9, 11, 13 }, { 1000, 1002, 1251 },
    };
    for (const Case& testCase : cases) {
        AOT_TEST_CHECK(DataNodeHeader::numberOfWords(Kind::Array, testCase.count) == testCase.wordsOfArray);
        AOT_TEST_CHECK(DataNodeHeader::numberOfWords(Kind::Object, testCase.count) == testCase.wordsOfObject);
        AOT_TEST_CHECK(DataNodeHeader::numberOfWords(Kind::ObjectWithOtherMembers, testCase.count) == testCase.wordsOfObject + 1);
        AOT_TEST_CHECK(DataNodeHeader::numberOfWords(Kind::OtherMembers, testCase.count) == 1 + testCase.count + (testCase.count + 1) / 2);
        AOT_TEST_CHECK(DataNodeHeader::numberOfWords(Kind::OrdinaryObject, testCase.count) == DataNodeHeader::numberOfWords(Kind::OtherMembers, testCase.count));
        AOT_TEST_CHECK(DataNodeHeader::numberOfWords(Kind::ObjectBody, testCase.count) == 1 + testCase.count);
    }

    for (Kind kind : { Kind::Array, Kind::Object, Kind::ObjectBody }) {
        DataNodeHeader header { 12, static_cast<uint32_t>(kind), 345 };
        DataNodeHeader decoded = DataNodeHeader::decode(header.encoded());
        AOT_TEST_CHECK(decoded.count == 12 && decoded.kind() == kind && decoded.operand == 345);
    }
}

static const DataRequest& requestFor(LoadedProgram& program, DataNumber number)
{
    std::span<const uint32_t> requests = program.state().tables.requests();
    AOT_TEST_CHECK(requests.size() == 1 + NumberOfData * sizeof(DataRequest) / sizeof(uint32_t));
    return reinterpret_cast<const DataRequest*>(requests.data() + 1)[number];
}

static JSObject* build(LoadedProgram& program, DataNumber number)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(program.vm());
    JSObject* result = tryBuildData(program.realm(), program.state(), requestFor(program, number));
    AOT_TEST_CHECK(!scope.exception() && result);
    return result;
}

static JSValue callInPlainRealm(LoadedProgram& program, ASCIILiteral functionSource, std::initializer_list<JSValue> values)
{
    VM& vm = program.vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSGlobalObject* realm = program.realms().plain();
    JSValue function = program.realms().evaluate(realm, functionSource);
    MarkedArgumentBuffer arguments;
    for (JSValue value : values)
        arguments.append(value);
    JSValue result = call(realm, function, arguments, "the source is a function"_s);
    AOT_TEST_CHECK_FOR_SOURCE(!scope.exception(), functionSource);
    return result;
}

static String describe(LoadedProgram& program, JSValue value)
{
    return callInPlainRealm(program, describeSource, { value }).toWTFString(program.realms().plain());
}

static bool isDescribedAs(LoadedProgram& program, JSValue value, ASCIILiteral expected)
{
    String description = describe(program, value);
    if (description == expected)
        return true;
    dataLogLn("built    ", description, "\nexpected ", expected);
    return false;
}

static JSValue get(LoadedProgram& program, JSValue base, ASCIILiteral name)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(program.vm());
    JSValue result = base.get(program.realm(), Identifier::fromString(program.vm(), name));
    AOT_TEST_CHECK(!scope.exception() && result);
    return result;
}

static JSValue getIndex(LoadedProgram& program, JSValue base, unsigned index)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(program.vm());
    JSValue result = base.get(program.realm(), index);
    AOT_TEST_CHECK(!scope.exception() && result);
    return result;
}

static void testRequestsOfTheProgram(LoadedProgram& program)
{
    std::span<const uint64_t> data = program.state().tables.data();
    std::span<const StaticsRange> roots = program.state().tables.staticsRoots();
    AOT_TEST_CHECK(requestFor(program, Tree).sharedStorageOffset == DataRequest::noSharedStorage);
    AOT_TEST_CHECK(DataNodeHeader::decode(data[requestFor(program, Tree).root]).kind() == DataNodeHeader::Kind::ObjectBody);
    for (DataNumber number : { Integers, Doubles, Mixture }) {
        DataNodeHeader header = DataNodeHeader::decode(data[requestFor(program, number).root]);
        AOT_TEST_CHECK(header.kind() == DataNodeHeader::Kind::Array && header.count == 17);
        AOT_TEST_CHECK(data[requestFor(program, number).root + 1] == (17 | 17ull << 32));
    }
    // Storage without cells is the node itself. Compiled code has the places of such nodes, which come first.
    AOT_TEST_CHECK(requestFor(program, Integers).sharedStorageOffset == DataRequest::noSharedStorage && requestFor(program, Integers).root == DataNodeHeader::firstNode);
    AOT_TEST_CHECK(requestFor(program, Doubles).sharedStorageOffset == DataRequest::noSharedStorage && requestFor(program, Doubles).root == DataNodeHeader::firstNode + DataNodeHeader::numberOfWords(DataNodeHeader::Kind::Array, 17));
    AOT_TEST_CHECK(std::ranges::any_of(roots, [&](const StaticsRange& range) {
        return range.contains(requestFor(program, Mixture).sharedStorageOffset);
    }));
    AOT_TEST_CHECK(DataNodeHeader::decode(data[requestFor(program, Integers).root]).operand == (ArrayWithInt32 | DataNodeHeader::isStorage));
    AOT_TEST_CHECK(DataNodeHeader::decode(data[requestFor(program, Doubles).root]).operand == (ArrayWithDouble | DataNodeHeader::isStorage));
    AOT_TEST_CHECK(DataNodeHeader::decode(data[requestFor(program, Mixture).root]).operand == ArrayWithContiguous);
}

static void testConstantOfDataWord(LoadedProgram& program)
{
    VM& vm = program.vm();
    State& state = program.state();
    AOT_TEST_CHECK(!constantOfDataWord(state, DataWord::hole));
    AOT_TEST_CHECK(constantOfDataWord(state, DataWord::encodeImmediate(jsUndefined())).isUndefined());
    AOT_TEST_CHECK(constantOfDataWord(state, DataWord::encodeImmediate(jsNull())).isNull());
    AOT_TEST_CHECK(constantOfDataWord(state, DataWord::encodeImmediate(jsBoolean(true))).isTrue());
    AOT_TEST_CHECK(constantOfDataWord(state, DataWord::encodeImmediate(jsNumber(-7))) == jsNumber(-7));
    JSValue negativeZero = constantOfDataWord(state, DataWord::encodeImmediate(jsDoubleNumber(-0.0)));
    AOT_TEST_CHECK(negativeZero.isDouble() && !negativeZero.asDouble() && std::signbit(negativeZero.asDouble()));

    uint32_t numberOfConstants = Image::current()->text().numberOfStringConstants();
    AOT_TEST_CHECK(numberOfConstants >= 5);
    for (uint32_t number = 0; number < numberOfConstants; ++number) {
        JSValue constant = constantOfDataWord(state, DataWord::encodeStringConstant(number));
        AOT_TEST_CHECK(constant.isString() && constant == JSValue(state.strings->constant(vm, number)));
        AOT_TEST_CHECK(constantOfDataWord(state, DataWord::encodeStringConstant(number)) == constant);
    }

    AOT_TEST_CHECK(state.tables.bigIntConstants().size() == 1);
    JSValue big = constantOfDataWord(state, DataWord::encodeBigIntConstant(0));
    AOT_TEST_CHECK(big.isBigInt() && big == state.strings->bigIntConstant(program.realm(), 0));
    AOT_TEST_CHECK(isDescribedAs(program, big, "12345678901234567890n"_s));
}

static void testNestedObjectsAndArrays(LoadedProgram& program)
{
    JSObject* tree = build(program, Tree);
    AOT_TEST_CHECK(isDescribedAs(program, tree, describedTree));

    AOT_TEST_CHECK(PropertyWalk::isCompiledObject(tree));
    AOT_TEST_CHECK(PropertyWalk::isCompiledObject(asObject(get(program, tree, "inner"_s))));
    AOT_TEST_CHECK(tree->realm() == program.realm());

    auto* mixed = uncheckedDowncast<JSArray>(get(program, tree, "mixed"_s));
    auto* doubles = uncheckedDowncast<JSArray>(get(program, tree, "doubles"_s));
    auto* integers = uncheckedDowncast<JSArray>(get(program, tree, "integers"_s));
    AOT_TEST_CHECK(mixed->indexingType() == ArrayWithContiguous && mixed->length() == 7);
    AOT_TEST_CHECK(doubles->indexingType() == ArrayWithDouble && doubles->length() == 4);
    // The readers of an array that is declared to hold numbers expect Double storage, also where every constant is a whole number.
    AOT_TEST_CHECK(integers->indexingType() == ArrayWithDouble && integers->length() == 2);
    AOT_TEST_CHECK(mixed->realm() == program.realm());
    AOT_TEST_CHECK(!mixed->canGetIndexQuickly(1u) && mixed->canGetIndexQuickly(4u) && mixed->getIndexQuickly(4).isUndefined());
    AOT_TEST_CHECK(!doubles->canGetIndexQuickly(1u) && doubles->canGetIndexQuickly(3u));
    AOT_TEST_CHECK(!isCopyOnWrite(mixed->indexingMode()) && !JSCellButterfly::isOutsideHeap(mixed->butterfly()));
    for (JSArray* array : { doubles, integers })
        AOT_TEST_CHECK(isCopyOnWrite(array->indexingMode()) && JSCellButterfly::isOutsideHeap(array->butterfly()));
}

static void testTwoExecutionsGiveDistinctGraphs(LoadedProgram& program)
{
    JSObject* first = build(program, Tree);
    JSObject* second = build(program, Tree);
    AOT_TEST_CHECK(first != second);
    for (ASCIILiteral name : { "mixed"_s, "doubles"_s, "integers"_s, "inner"_s, "nested"_s }) {
        JSValue inFirst = get(program, first, name);
        JSValue inSecond = get(program, second, name);
        AOT_TEST_CHECK(inFirst.isObject() && inSecond.isObject() && inFirst != inSecond);
        if (auto* array = dynamicDowncast<JSArray>(inFirst))
            AOT_TEST_CHECK((array->butterfly() == uncheckedDowncast<JSArray>(inSecond)->butterfly()) == isCopyOnWrite(array->indexingMode()));
    }
    JSValue deepInFirst = getIndex(program, getIndex(program, get(program, first, "nested"_s), 0), 0);
    JSValue deepInSecond = getIndex(program, getIndex(program, get(program, second, "nested"_s), 0), 0);
    AOT_TEST_CHECK(deepInFirst.isObject() && deepInFirst != deepInSecond);
    AOT_TEST_CHECK(get(program, first, "text"_s) == get(program, second, "text"_s));
    AOT_TEST_CHECK(get(program, first, "big"_s) == get(program, second, "big"_s));

    callInPlainRealm(program, "(function (tree) { tree.mixed[0] = 'changed'; tree.doubles[1] = 4; tree.integers.push(3); tree.inner.zero = 1; tree.nested[0][0].nan = 2; tree.text = 'other'; })"_s, { first });
    AOT_TEST_CHECK(isDescribedAs(program, first, "{mixed:[\"changed\",hole,\"pear\",true,undefined,null,false],doubles:[1.5,4,2,-0],integers:[1,2,3],inner:{zero:1,nan:NaN,nothing:null},big:12345678901234567890n,text:\"other\",nested:[[{zero:7,nan:2,nothing:undefined}],[]]}"_s));
    AOT_TEST_CHECK(isDescribedAs(program, second, describedTree));
    AOT_TEST_CHECK(isDescribedAs(program, build(program, Tree), describedTree));
}

static Object* compiledObject(JSValue value)
{
    AOT_TEST_CHECK(value.isObject() && PropertyWalk::isCompiledObject(asObject(value)));
    return uncheckedDowncast<Object>(asObject(value));
}

static void testMembersThatAreNoFields(LoadedProgram& program)
{
    std::span<const uint64_t> data = program.state().tables.data();
    AOT_TEST_CHECK(DataNodeHeader::decode(data[requestFor(program, Dictionary).root]).kind() == DataNodeHeader::Kind::ObjectWithOtherMembers);

    Object* first = compiledObject(build(program, Dictionary));
    Object* second = compiledObject(build(program, Dictionary));
    AOT_TEST_CHECK(first != second && first->extraProperties() && first->extraProperties() != second->extraProperties());
    AOT_TEST_CHECK(isDescribedAs(program, first, describedDictionary) && isDescribedAs(program, second, describedDictionary));
    AOT_TEST_CHECK(!compiledObject(get(program, first, "empty"_s))->extraProperties());

    Object* nestedInFirst = compiledObject(get(program, first, "nested"_s));
    Object* nestedInSecond = compiledObject(get(program, second, "nested"_s));
    AOT_TEST_CHECK(nestedInFirst != nestedInSecond && nestedInFirst->structure() == nestedInSecond->structure());
    AOT_TEST_CHECK(nestedInFirst->extraProperties()->structure() == nestedInSecond->extraProperties()->structure());
    AOT_TEST_CHECK(!nestedInFirst->extraProperties()->structure()->isDictionary());

    callInPlainRealm(program, "(function (record) { delete record.w0; record['content-type'] = 'other'; record.added = 1; record[1] = 'one'; record.nested.deep = 0; record.w0 = 'again'; })"_s, { first });
    AOT_TEST_CHECK(isDescribedAs(program, first, "{1:\"one\",2:-0,10:\"ten\",name:\"fig\",size:3,content-type:\"other\",nested:{deep:0,constructor:null},empty:{},w1:1,w2:2,w3:3,w4:4,w5:5,w6:6,w7:7,w8:8,w9:9,w10:10,w11:11,added:1,w0:\"again\"}"_s));
    AOT_TEST_CHECK(isDescribedAs(program, second, describedDictionary));
    AOT_TEST_CHECK(isDescribedAs(program, build(program, Dictionary), describedDictionary));
    AOT_TEST_CHECK(callInPlainRealm(program, "(function (record) { let d = Object.getOwnPropertyDescriptor(record, 'content-type'); return d.writable && d.enumerable && d.configurable && Object.isExtensible(record) && JSON.stringify(record.nested.constructor) === 'null'; })"_s, { second }).isTrue());
}

static void testManyMembersBecomeOneDictionary(LoadedProgram& program)
{
    StringBuilder expected;
    expected.append('{');
    for (unsigned index = 0; index < numberOfKeysInLargeDictionary; ++index) {
        if (index)
            expected.append(',');
        if (index % 2)
            expected.append("key "_s, index, ":\"v"_s, index, '"');
        else
            expected.append("key "_s, index, ':', index);
    }
    expected.append('}');
    String described = expected.toString();

    Object* first = compiledObject(build(program, LargeDictionary));
    Object* second = compiledObject(build(program, LargeDictionary));
    AOT_TEST_CHECK(first->extraProperties()->structure()->isDictionary() && first->extraProperties()->structure() != second->extraProperties()->structure());
    AOT_TEST_CHECK(describe(program, first) == described && describe(program, second) == described);
    AOT_TEST_CHECK(get(program, first, "key 129"_s) == get(program, second, "key 129"_s));

    callInPlainRealm(program, "(function (record) { for (let key of Object.keys(record)) delete record[key]; })"_s, { first });
    AOT_TEST_CHECK(isDescribedAs(program, first, "{}"_s));
    AOT_TEST_CHECK(describe(program, second) == described);
}

static void testObjectThatOnlyUnknownCodeSees(LoadedProgram& program)
{
    JSObject* first = build(program, SeenByUnknownCodeOnly);
    JSObject* second = build(program, SeenByUnknownCodeOnly);
    AOT_TEST_CHECK(first != second);
    AOT_TEST_CHECK(isDescribedAs(program, first, describedOrdinaryObject) && isDescribedAs(program, second, describedOrdinaryObject));
}

static JSObject* buildRequest(LoadedProgram& program, unsigned number)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(program.vm());
    JSObject* result = tryBuildData(program.realm(), program.state(), reinterpret_cast<const DataRequest*>(program.state().tables.requests().data() + 1)[number]);
    AOT_TEST_CHECK(!scope.exception() && result);
    return result;
}

static void testOrdinaryObjects(LoadedProgram& program)
{
    std::span<const uint64_t> data = program.state().tables.data();
    std::span<const uint32_t> requests = program.state().tables.requests();
    AOT_TEST_CHECK(requests.size() == 1 + 2 * sizeof(DataRequest) / sizeof(uint32_t));
    for (unsigned number = 0; number < 2; ++number)
        AOT_TEST_CHECK(DataNodeHeader::decode(data[reinterpret_cast<const DataRequest*>(requests.data() + 1)[number].root]).kind() == DataNodeHeader::Kind::OrdinaryObject);

    JSObject* few = buildRequest(program, 0);
    JSObject* fewAgain = buildRequest(program, 0);
    for (JSObject* object : { few, fewAgain })
        AOT_TEST_CHECK(object->type() == FinalObjectType && !PropertyWalk::isCompiledObject(object) && !object->structure()->isDictionary() && object->getPrototypeDirect() == program.realm()->objectPrototype());
    AOT_TEST_CHECK(few != fewAgain);
    AOT_TEST_CHECK(isDescribedAs(program, few, "{one:1,content-type:\"two\",nothing:null}"_s) && isDescribedAs(program, fewAgain, "{one:1,content-type:\"two\",nothing:null}"_s));

    StringBuilder expected;
    expected.append('{');
    for (unsigned index = 0; index < numberOfNamesInLargeLiteral; ++index) {
        static constexpr ASCIILiteral values[] = { "1"_s, "\"two\""_s, "null"_s };
        expected.append(index ? ","_s : ""_s, "name "_s, index, ':', values[index % 3]);
    }
    expected.append('}');
    String described = expected.toString();

    JSObject* many = buildRequest(program, 1);
    JSObject* manyAgain = buildRequest(program, 1);
    AOT_TEST_CHECK(many->structure()->isDictionary() && !many->structure()->isUncacheableDictionary() && many->structure() != manyAgain->structure());
    AOT_TEST_CHECK(describe(program, many) == described && describe(program, manyAgain) == described);
    AOT_TEST_CHECK(get(program, many, "name 1"_s) == get(program, manyAgain, "name 1"_s));

    callInPlainRealm(program, "(function (few, many) { few.added = 1; delete few.one; few.nothing = 'stored'; many['name 3'] = 'stored'; many.added = 2; many[4] = 'index'; delete many['name 0']; })"_s, { few, many });
    AOT_TEST_CHECK(isDescribedAs(program, few, "{content-type:\"two\",nothing:\"stored\",added:1}"_s) && isDescribedAs(program, fewAgain, "{one:1,content-type:\"two\",nothing:null}"_s));
    AOT_TEST_CHECK(describe(program, manyAgain) == described && describe(program, buildRequest(program, 1)) == described);
    AOT_TEST_CHECK(callInPlainRealm(program, "(function (many) { let keys = Object.keys(many); return keys.length === 131 && keys[0] === '4' && keys[1] === 'name 1' && keys[130] === 'added' && many['name 3'] === 'stored'; })"_s, { many }).isTrue());
    for (JSObject* object : { fewAgain, manyAgain })
        AOT_TEST_CHECK(callInPlainRealm(program, "(function (object) { return Object.values(Object.getOwnPropertyDescriptors(object)).every(d => d.writable && d.enumerable && d.configurable) && Object.isExtensible(object); })"_s, { object }).isTrue());

    program.vm().heap.collectNow(Sync, CollectionScope::Full);
    AOT_TEST_CHECK(describe(program, manyAgain) == described && isDescribedAs(program, fewAgain, "{one:1,content-type:\"two\",nothing:null}"_s));
}

static void checkSharedStorage(LoadedProgram& program, DataNumber number, IndexingType expectedMode, ASCIILiteral described, ASCIILiteral describedAfterStore)
{
    auto& shared = program.state().statics->at<JSCellButterfly*>(requestFor(program, number).sharedStorageOffset);
    AOT_TEST_CHECK(!shared);

    auto* first = uncheckedDowncast<JSArray>(build(program, number));
    JSCellButterfly* storage = shared;
    AOT_TEST_CHECK(storage && storage->indexingMode() == expectedMode && storage->length() == 17);
    auto* second = uncheckedDowncast<JSArray>(build(program, number));
    AOT_TEST_CHECK(shared == storage);

    AOT_TEST_CHECK(first != second);
    for (JSArray* array : { first, second }) {
        AOT_TEST_CHECK(array->indexingMode() == expectedMode && isCopyOnWrite(array->indexingMode()));
        AOT_TEST_CHECK(array->butterfly() == storage->toButterfly());
        AOT_TEST_CHECK(array->realm() == program.realm());
        AOT_TEST_CHECK(isDescribedAs(program, array, described));
    }

    callInPlainRealm(program, "(function (array) { array[1] = 99; })"_s, { first });
    AOT_TEST_CHECK(!isCopyOnWrite(first->indexingMode()) && first->butterfly() != storage->toButterfly());
    AOT_TEST_CHECK(isDescribedAs(program, first, describedAfterStore));
    AOT_TEST_CHECK(second->butterfly() == storage->toButterfly() && isCopyOnWrite(second->indexingMode()));
    AOT_TEST_CHECK(isDescribedAs(program, second, described));

    auto* third = uncheckedDowncast<JSArray>(build(program, number));
    AOT_TEST_CHECK(shared == storage && third->butterfly() == storage->toButterfly());
    AOT_TEST_CHECK(isDescribedAs(program, third, described));
}

static void checkStorageInImage(LoadedProgram& program, DataNumber number, IndexingType expectedMode, ASCIILiteral described, ASCIILiteral describedAfterStore)
{
    VM& vm = program.vm();
    auto* storage = std::bit_cast<Butterfly*>(program.state().tables.data().data() + requestFor(program, number).root + DataNodeHeader::wordsBeforeElements);
    AOT_TEST_CHECK(JSCellButterfly::isOutsideHeap(storage) && storage->publicLength() == 17 && storage->vectorLength() == 17);

    auto* first = uncheckedDowncast<JSArray>(build(program, number));
    auto* second = uncheckedDowncast<JSArray>(build(program, number));
    AOT_TEST_CHECK(first != second);
    for (JSArray* array : { first, second }) {
        AOT_TEST_CHECK(array->indexingMode() == expectedMode && array->butterfly() == storage && array->realm() == program.realm());
        AOT_TEST_CHECK(isDescribedAs(program, array, described));
    }

    callInPlainRealm(program, "(function (array) { array[1] = 99; })"_s, { first });
    AOT_TEST_CHECK(!isCopyOnWrite(first->indexingMode()) && !JSCellButterfly::isOutsideHeap(first->butterfly()));
    AOT_TEST_CHECK(isDescribedAs(program, first, describedAfterStore));
    for (CollectionScope scope : { CollectionScope::Eden, CollectionScope::Full }) {
        vm.heap.collectSync(scope);
        AOT_TEST_CHECK(second->butterfly() == storage && isDescribedAs(program, second, described) && isDescribedAs(program, first, describedAfterStore));
    }
    AOT_TEST_CHECK(isDescribedAs(program, callInPlainRealm(program, "(function (array) { let copy = array.slice(); copy.reverse(); copy.length = 2; return array; })"_s, { second }), described));
    AOT_TEST_CHECK(isDescribedAs(program, build(program, number), described));
}

static void testCopyOnWriteArrayOfIntegers(LoadedProgram& program)
{
    checkStorageInImage(program, Integers, CopyOnWriteArrayWithInt32, describedIntegers, "[1,99,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17]"_s);
}

static void testCopyOnWriteArrayOfDoubles(LoadedProgram& program)
{
    checkStorageInImage(program, Doubles, CopyOnWriteArrayWithDouble, describedDoubles, "[0.5,99,3,4,5,6,7,8,9,10,11,12,13,14,15,16,-0]"_s);
}

static void testCopyOnWriteArrayOfAnything(LoadedProgram& program)
{
    checkSharedStorage(program, Mixture, CopyOnWriteArrayWithContiguous, describedMixture, "[\"a0\",99,undefined,null,true,\"a5\",7,8,9,10,11,12,13,14,15,16,\"a16\"]"_s);
}

static void testSharedStorageSurvivesCollections(LoadedProgram& program)
{
    VM& vm = program.vm();
    auto& shared = program.state().statics->at<JSCellButterfly*>(requestFor(program, Mixture).sharedStorageOffset);
    build(program, Mixture);
    JSCellButterfly* storage = shared;
    AOT_TEST_CHECK(storage);
    for (CollectionScope scope : { CollectionScope::Eden, CollectionScope::Full, CollectionScope::Eden }) {
        vm.heap.collectSync(scope);
        AOT_TEST_CHECK(shared == storage && vm.heap.isMarked(storage));
        AOT_TEST_CHECK(isDescribedAs(program, build(program, Mixture), describedMixture));
    }
}

static void checkNothingIsLost(LoadedProgram& program)
{
    AOT_TEST_CHECK(Options::collectContinuously());
    for (unsigned round = 0; round < 3000; ++round) {
        JSObject* tree = build(program, Tree);
        JSObject* mixture = build(program, Mixture);
        JSObject* dictionary = build(program, Dictionary);
        if (round % 100)
            continue;
        AOT_TEST_CHECK(isDescribedAs(program, dictionary, describedDictionary));
        AOT_TEST_CHECK(isDescribedAs(program, tree, describedTree));
        AOT_TEST_CHECK(isDescribedAs(program, mixture, describedMixture));
        AOT_TEST_CHECK(isDescribedAs(program, build(program, Doubles), describedDoubles));
    }
}

static void testCollectionInTheMiddleLosesNothing()
{
    if (Options::aotImagePath()) {
        runWithLoadedProgram(collectionInTheMiddleTest, programText, checkNothingIsLost);
        return;
    }

    TemporaryFile image(".image"_s, compileProgramText(programText).span());
    ChildProcessLauncher launcher(String(programPath()), { });
    Vector<String> arguments {
        String(nativeSubcommandName), String(singleProcessFlag), String(collectionInTheMiddleTest),
        makeString("--aotImagePath="_s, image.path()), "--collectContinuously=1"_s,
    };
    ChildProcessResult result = launcher.run(arguments.span());
    if (!result.exitedWith(0)) {
        dataLogLn(result.output);
        dataLogLn("The process that collects continuously ", result.description());
        failNativeTest(__FILE__, __LINE__, collectionInTheMiddleTest.characters());
    }
}

template<LoadedProgramTest body>
static constexpr void (*withData)() = runNativeTestWithLoadedProgram<dataBuilderTests, programText, body>;

template<LoadedProgramTest body>
static constexpr void (*withDictionaryLiterals)() = runNativeTestWithLoadedProgram<dataBuilderTests, programWithDictionaryLiteralsText, body>;

std::span<const NativeTest> dataBuilderTests()
{
    static constexpr NativeTest tests[] = {
        { "data-builder/data-words-round-trip"_s, testDataWordsRoundTrip },
        { "data-builder/number-of-words"_s, testNumberOfWords },
        { "data-builder/requests-of-the-program"_s, withData<testRequestsOfTheProgram> },
        { "data-builder/constant-of-data-word"_s, withData<testConstantOfDataWord> },
        { "data-builder/nested-objects-and-arrays"_s, withData<testNestedObjectsAndArrays> },
        { "data-builder/two-executions-give-distinct-graphs"_s, withData<testTwoExecutionsGiveDistinctGraphs> },
        { "data-builder/members-that-are-no-fields"_s, withData<testMembersThatAreNoFields> },
        { "data-builder/many-members-become-one-dictionary"_s, withData<testManyMembersBecomeOneDictionary> },
        { "data-builder/object-that-only-unknown-code-sees"_s, withData<testObjectThatOnlyUnknownCodeSees> },
        { "data-builder/ordinary-objects"_s, withDictionaryLiterals<testOrdinaryObjects> },
        { "data-builder/copy-on-write-array-of-integers"_s, withData<testCopyOnWriteArrayOfIntegers> },
        { "data-builder/copy-on-write-array-of-doubles"_s, withData<testCopyOnWriteArrayOfDoubles> },
        { "data-builder/copy-on-write-array-of-anything"_s, withData<testCopyOnWriteArrayOfAnything> },
        { "data-builder/shared-storage-survives-collections"_s, withData<testSharedStorageSurvivesCollections> },
        { collectionInTheMiddleTest, testCollectionInTheMiddleLosesNothing },
    };
    return tests;
}

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
