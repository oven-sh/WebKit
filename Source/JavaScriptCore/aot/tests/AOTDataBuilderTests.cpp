/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTChildProcess.h"
#include "AOTImage.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTObject.h"
#include "AOTOperationData.h"
#include "AOTProgramStatics.h"
#include "AOTProgramStrings.h"
#include "AOTPropertyWalk.h"
#include "AOTRuntimeFunctions.h"
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
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="publish" [isFreeName]
    v2 = Data data=d0 @10
    v3 = Call v1, -, v2
    v4 = Data data=d1 @20
    v5 = Call v1, -, v4
    v6 = Data data=d2 @30
    v7 = Call v1, -, v6
    v8 = NewClosure function=f1
    v9 = Call v8, -, v0 [isNeverInlined]
    v10 = Call v1, -, v9
    v11 = Data data=d4 @50
    v12 = Call v1, -, v11
    ElemSet v11, v12, v12 mode=Strict
    v14 = Data data=d5 @60
    v15 = Call v1, -, v14
    ElemSet v14, v15, v15 mode=Strict
    v17 = Data data=d6 @70
    v18 = Call v1, -, v17
    v19 = Call v8, -, v0 [isNeverInlined]
    Return
}
function f1 name="runsTwice" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = Data data=d3 @40
    Return v1
}
data d0 root=Object{layout=L0 members=[{key="mixed" value=Array{contents=anything elements=[Number{value=1}, Hole, String{value="pear"}, Boolean{value=true}, Undefined, Null, Boolean{}]}}, {key="doubles" value=Array{contents=number elements=[Number{value=1.5}, Hole, Number{value=2}, Number{value=-0}]}}, {key="integers" value=Array{contents=number elements=[Number{value=1}, Number{value=2}]}}, {key="inner" value=Object{layout=L1 members=[{key="zero" value=Number{value=-0}}, {key="nan" value=Number{value=NaN}}, {key="nothing" value=Null}]}}, {key="big" value=BigInt{digits="12345678901234567890"}}, {key="text" value=String{value="plum"}}, {key="nested" value=Array{contents=anything elements=[Array{contents=anything elements=[Object{layout=L1 members=[{key="zero" value=Number{value=7}}, {key="nan" value=Number{value=8}}, {key="nothing" value=Undefined}]}]}, Array{contents=anything}]}}]} module=m0
data d1 root=Array{contents=i32 elements=[Number{value=1}, Number{value=2}, Number{value=3}, Number{value=4}, Number{value=5}, Number{value=6}, Number{value=7}, Number{value=8}, Number{value=9}, Number{value=10}, Number{value=11}, Number{value=12}, Number{value=13}, Number{value=14}, Number{value=15}, Number{value=16}, Number{value=17}]} module=m0
data d2 root=Array{contents=number elements=[Number{value=0.5}, Number{value=2}, Number{value=3}, Number{value=4}, Number{value=5}, Number{value=6}, Number{value=7}, Number{value=8}, Number{value=9}, Number{value=10}, Number{value=11}, Number{value=12}, Number{value=13}, Number{value=14}, Number{value=15}, Number{value=16}, Number{value=-0}]} module=m0
data d3 root=Array{contents=anything elements=[String{value="a0"}, Number{value=2}, Undefined, Null, Boolean{value=true}, String{value="a5"}, Number{value=7}, Number{value=8}, Number{value=9}, Number{value=10}, Number{value=11}, Number{value=12}, Number{value=13}, Number{value=14}, Number{value=15}, Number{value=16}, String{value="a16"}]} module=m0
data d4 root=Object{layout=L3 members=[{key="name" value=String{value="fig"}}, {key="10" value=String{value="ten"}}, {key="size" value=Number{value=3}}, {key="content-type" value=String{value="text"}}, {key="2" value=Number{value=-0}}, {key="nested" value=Object{layout=L2 members=[{key="-1.5" value=Array{contents=anything elements=[Object{layout=L2 members=[{key="deeper" value=BigInt{digits="12345678901234567890"}}]}]}}, {key="-2.5" value=Null}]}}, {key="empty" value=Object{layout=L2}}, {key="w0" value=Number{value=0}}, {key="w1" value=Number{value=1}}, {key="w2" value=Number{value=2}}, {key="w3" value=Number{value=3}}, {key="w4" value=Number{value=4}}, {key="w5" value=Number{value=5}}, {key="w6" value=Number{value=6}}, {key="w7" value=Number{value=7}}, {key="w8" value=Number{value=8}}, {key="w9" value=Number{value=9}}, {key="w10" value=Number{value=10}}, {key="w11" value=Number{value=11}}]} module=m0
data d5 root=Object{layout=L2 members=[{key="-1" value=Number{value=0}}, {key="-2" value=String{value="v1"}}, {key="-3" value=Number{value=2}}, {key="-4" value=String{value="v3"}}, {key="-5" value=Number{value=4}}, {key="-6" value=String{value="v5"}}, {key="-7" value=Number{value=6}}, {key="-8" value=String{value="v7"}}, {key="-9" value=Number{value=8}}, {key="-10" value=String{value="v9"}}, {key="-11" value=Number{value=10}}, {key="-12" value=String{value="v11"}}, {key="-13" value=Number{value=12}}, {key="-14" value=String{value="v13"}}, {key="-15" value=Number{value=14}}, {key="-16" value=String{value="v15"}}, {key="-17" value=Number{value=16}}, {key="-18" value=String{value="v17"}}, {key="-19" value=Number{value=18}}, {key="-20" value=String{value="v19"}}, {key="-21" value=Number{value=20}}, {key="-22" value=String{value="v21"}}, {key="-23" value=Number{value=22}}, {key="-24" value=String{value="v23"}}, {key="-25" value=Number{value=24}}, {key="-26" value=String{value="v25"}}, {key="-27" value=Number{value=26}}, {key="-28" value=String{value="v27"}}, {key="-29" value=Number{value=28}}, {key="-30" value=String{value="v29"}}, {key="-31" value=Number{value=30}}, {key="-32" value=String{value="v31"}}, {key="-33" value=Number{value=32}}, {key="-34" value=String{value="v33"}}, {key="-35" value=Number{value=34}}, {key="-36" value=String{value="v35"}}, {key="-37" value=Number{value=36}}, {key="-38" value=String{value="v37"}}, {key="-39" value=Number{value=38}}, {key="-40" value=String{value="v39"}}, {key="-41" value=Number{value=40}}, {key="-42" value=String{value="v41"}}, {key="-43" value=Number{value=42}}, {key="-44" value=String{value="v43"}}, {key="-45" value=Number{value=44}}, {key="-46" value=String{value="v45"}}, {key="-47" value=Number{value=46}}, {key="-48" value=String{value="v47"}}, {key="-49" value=Number{value=48}}, {key="-50" value=String{value="v49"}}, {key="-51" value=Number{value=50}}, {key="-52" value=String{value="v51"}}, {key="-53" value=Number{value=52}}, {key="-54" value=String{value="v53"}}, {key="-55" value=Number{value=54}}, {key="-56" value=String{value="v55"}}, {key="-57" value=Number{value=56}}, {key="-58" value=String{value="v57"}}, {key="-59" value=Number{value=58}}, {key="-60" value=String{value="v59"}}, {key="-61" value=Number{value=60}}, {key="-62" value=String{value="v61"}}, {key="-63" value=Number{value=62}}, {key="-64" value=String{value="v63"}}, {key="-65" value=Number{value=64}}, {key="-66" value=String{value="v65"}}, {key="-67" value=Number{value=66}}, {key="-68" value=String{value="v67"}}, {key="-69" value=Number{value=68}}, {key="-70" value=String{value="v69"}}, {key="-71" value=Number{value=70}}, {key="-72" value=String{value="v71"}}, {key="-73" value=Number{value=72}}, {key="-74" value=String{value="v73"}}, {key="-75" value=Number{value=74}}, {key="-76" value=String{value="v75"}}, {key="-77" value=Number{value=76}}, {key="-78" value=String{value="v77"}}, {key="-79" value=Number{value=78}}, {key="-80" value=String{value="v79"}}, {key="-81" value=Number{value=80}}, {key="-82" value=String{value="v81"}}, {key="-83" value=Number{value=82}}, {key="-84" value=String{value="v83"}}, {key="-85" value=Number{value=84}}, {key="-86" value=String{value="v85"}}, {key="-87" value=Number{value=86}}, {key="-88" value=String{value="v87"}}, {key="-89" value=Number{value=88}}, {key="-90" value=String{value="v89"}}, {key="-91" value=Number{value=90}}, {key="-92" value=String{value="v91"}}, {key="-93" value=Number{value=92}}, {key="-94" value=String{value="v93"}}, {key="-95" value=Number{value=94}}, {key="-96" value=String{value="v95"}}, {key="-97" value=Number{value=96}}, {key="-98" value=String{value="v97"}}, {key="-99" value=Number{value=98}}, {key="-100" value=String{value="v99"}}, {key="-101" value=Number{value=100}}, {key="-102" value=String{value="v101"}}, {key="-103" value=Number{value=102}}, {key="-104" value=String{value="v103"}}, {key="-105" value=Number{value=104}}, {key="-106" value=String{value="v105"}}, {key="-107" value=Number{value=106}}, {key="-108" value=String{value="v107"}}, {key="-109" value=Number{value=108}}, {key="-110" value=String{value="v109"}}, {key="-111" value=Number{value=110}}, {key="-112" value=String{value="v111"}}, {key="-113" value=Number{value=112}}, {key="-114" value=String{value="v113"}}, {key="-115" value=Number{value=114}}, {key="-116" value=String{value="v115"}}, {key="-117" value=Number{value=116}}, {key="-118" value=String{value="v117"}}, {key="-119" value=Number{value=118}}, {key="-120" value=String{value="v119"}}, {key="-121" value=Number{value=120}}, {key="-122" value=String{value="v121"}}, {key="-123" value=Number{value=122}}, {key="-124" value=String{value="v123"}}, {key="-125" value=Number{value=124}}, {key="-126" value=String{value="v125"}}, {key="-127" value=Number{value=126}}, {key="-128" value=String{value="v127"}}, {key="-129" value=Number{value=128}}, {key="-130" value=String{value="v129"}}, {key="-131" value=Number{value=130}}, {key="-132" value=String{value="v131"}}, {key="-133" value=Number{value=132}}, {key="-134" value=String{value="v133"}}, {key="-135" value=Number{value=134}}, {key="-136" value=String{value="v135"}}, {key="-137" value=Number{value=136}}, {key="-138" value=String{value="v137"}}, {key="-139" value=Number{value=138}}, {key="-140" value=String{value="v139"}}, {key="-141" value=Number{value=140}}, {key="-142" value=String{value="v141"}}, {key="-143" value=Number{value=142}}, {key="-144" value=String{value="v143"}}, {key="-145" value=Number{value=144}}, {key="-146" value=String{value="v145"}}, {key="-147" value=Number{value=146}}, {key="-148" value=String{value="v147"}}, {key="-149" value=Number{value=148}}, {key="-150" value=String{value="v149"}}, {key="-151" value=Number{value=150}}, {key="-152" value=String{value="v151"}}, {key="-153" value=Number{value=152}}, {key="-154" value=String{value="v153"}}, {key="-155" value=Number{value=154}}, {key="-156" value=String{value="v155"}}, {key="-157" value=Number{value=156}}, {key="-158" value=String{value="v157"}}, {key="-159" value=Number{value=158}}, {key="-160" value=String{value="v159"}}, {key="-161" value=Number{value=160}}, {key="-162" value=String{value="v161"}}, {key="-163" value=Number{value=162}}, {key="-164" value=String{value="v163"}}, {key="-165" value=Number{value=164}}, {key="-166" value=String{value="v165"}}, {key="-167" value=Number{value=166}}, {key="-168" value=String{value="v167"}}, {key="-169" value=Number{value=168}}, {key="-170" value=String{value="v169"}}, {key="-171" value=Number{value=170}}, {key="-172" value=String{value="v171"}}, {key="-173" value=Number{value=172}}, {key="-174" value=String{value="v173"}}, {key="-175" value=Number{value=174}}, {key="-176" value=String{value="v175"}}, {key="-177" value=Number{value=176}}, {key="-178" value=String{value="v177"}}, {key="-179" value=Number{value=178}}, {key="-180" value=String{value="v179"}}, {key="-181" value=Number{value=180}}, {key="-182" value=String{value="v181"}}, {key="-183" value=Number{value=182}}, {key="-184" value=String{value="v183"}}, {key="-185" value=Number{value=184}}, {key="-186" value=String{value="v185"}}, {key="-187" value=Number{value=186}}, {key="-188" value=String{value="v187"}}, {key="-189" value=Number{value=188}}, {key="-190" value=String{value="v189"}}, {key="-191" value=Number{value=190}}, {key="-192" value=String{value="v191"}}, {key="-193" value=Number{value=192}}, {key="-194" value=String{value="v193"}}, {key="-195" value=Number{value=194}}, {key="-196" value=String{value="v195"}}, {key="-197" value=Number{value=196}}, {key="-198" value=String{value="v197"}}, {key="-199" value=Number{value=198}}, {key="-200" value=String{value="v199"}}, {key="-201" value=Number{value=200}}, {key="-202" value=String{value="v201"}}, {key="-203" value=Number{value=202}}, {key="-204" value=String{value="v203"}}, {key="-205" value=Number{value=204}}, {key="-206" value=String{value="v205"}}, {key="-207" value=Number{value=206}}, {key="-208" value=String{value="v207"}}, {key="-209" value=Number{value=208}}, {key="-210" value=String{value="v209"}}, {key="-211" value=Number{value=210}}, {key="-212" value=String{value="v211"}}, {key="-213" value=Number{value=212}}, {key="-214" value=String{value="v213"}}, {key="-215" value=Number{value=214}}, {key="-216" value=String{value="v215"}}, {key="-217" value=Number{value=216}}, {key="-218" value=String{value="v217"}}, {key="-219" value=Number{value=218}}, {key="-220" value=String{value="v219"}}, {key="-221" value=Number{value=220}}, {key="-222" value=String{value="v221"}}, {key="-223" value=Number{value=222}}, {key="-224" value=String{value="v223"}}, {key="-225" value=Number{value=224}}, {key="-226" value=String{value="v225"}}, {key="-227" value=Number{value=226}}, {key="-228" value=String{value="v227"}}, {key="-229" value=Number{value=228}}, {key="-230" value=String{value="v229"}}, {key="-231" value=Number{value=230}}, {key="-232" value=String{value="v231"}}, {key="-233" value=Number{value=232}}, {key="-234" value=String{value="v233"}}, {key="-235" value=Number{value=234}}, {key="-236" value=String{value="v235"}}, {key="-237" value=Number{value=236}}, {key="-238" value=String{value="v237"}}, {key="-239" value=Number{value=238}}, {key="-240" value=String{value="v239"}}, {key="-241" value=Number{value=240}}, {key="-242" value=String{value="v241"}}, {key="-243" value=Number{value=242}}, {key="-244" value=String{value="v243"}}, {key="-245" value=Number{value=244}}, {key="-246" value=String{value="v245"}}, {key="-247" value=Number{value=246}}, {key="-248" value=String{value="v247"}}, {key="-249" value=Number{value=248}}, {key="-250" value=String{value="v249"}}, {key="-251" value=Number{value=250}}, {key="-252" value=String{value="v251"}}, {key="-253" value=Number{value=252}}, {key="-254" value=String{value="v253"}}, {key="-255" value=Number{value=254}}, {key="-256" value=String{value="v255"}}, {key="-257" value=Number{value=256}}, {key="-258" value=String{value="v257"}}, {key="-259" value=Number{value=258}}, {key="-260" value=String{value="v259"}}, {key="-261" value=Number{value=260}}, {key="-262" value=String{value="v261"}}, {key="-263" value=Number{value=262}}, {key="-264" value=String{value="v263"}}, {key="-265" value=Number{value=264}}, {key="-266" value=String{value="v265"}}, {key="-267" value=Number{value=266}}, {key="-268" value=String{value="v267"}}, {key="-269" value=Number{value=268}}, {key="-270" value=String{value="v269"}}, {key="-271" value=Number{value=270}}, {key="-272" value=String{value="v271"}}, {key="-273" value=Number{value=272}}, {key="-274" value=String{value="v273"}}, {key="-275" value=Number{value=274}}, {key="-276" value=String{value="v275"}}, {key="-277" value=Number{value=276}}, {key="-278" value=String{value="v277"}}, {key="-279" value=Number{value=278}}, {key="-280" value=String{value="v279"}}, {key="-281" value=Number{value=280}}, {key="-282" value=String{value="v281"}}, {key="-283" value=Number{value=282}}, {key="-284" value=String{value="v283"}}, {key="-285" value=Number{value=284}}, {key="-286" value=String{value="v285"}}, {key="-287" value=Number{value=286}}, {key="-288" value=String{value="v287"}}, {key="-289" value=Number{value=288}}, {key="-290" value=String{value="v289"}}, {key="-291" value=Number{value=290}}, {key="-292" value=String{value="v291"}}, {key="-293" value=Number{value=292}}, {key="-294" value=String{value="v293"}}, {key="-295" value=Number{value=294}}, {key="-296" value=String{value="v295"}}, {key="-297" value=Number{value=296}}, {key="-298" value=String{value="v297"}}, {key="-299" value=Number{value=298}}, {key="-300" value=String{value="v299"}}, {key="-301" value=Number{value=300}}, {key="-302" value=String{value="v301"}}, {key="-303" value=Number{value=302}}, {key="-304" value=String{value="v303"}}, {key="-305" value=Number{value=304}}, {key="-306" value=String{value="v305"}}, {key="-307" value=Number{value=306}}, {key="-308" value=String{value="v307"}}, {key="-309" value=Number{value=308}}, {key="-310" value=String{value="v309"}}, {key="-311" value=Number{value=310}}, {key="-312" value=String{value="v311"}}, {key="-313" value=Number{value=312}}, {key="-314" value=String{value="v313"}}, {key="-315" value=Number{value=314}}, {key="-316" value=String{value="v315"}}, {key="-317" value=Number{value=316}}, {key="-318" value=String{value="v317"}}, {key="-319" value=Number{value=318}}, {key="-320" value=String{value="v319"}}, {key="-321" value=Number{value=320}}, {key="-322" value=String{value="v321"}}, {key="-323" value=Number{value=322}}, {key="-324" value=String{value="v323"}}, {key="-325" value=Number{value=324}}, {key="-326" value=String{value="v325"}}, {key="-327" value=Number{value=326}}, {key="-328" value=String{value="v327"}}, {key="-329" value=Number{value=328}}, {key="-330" value=String{value="v329"}}, {key="-331" value=Number{value=330}}, {key="-332" value=String{value="v331"}}, {key="-333" value=Number{value=332}}, {key="-334" value=String{value="v333"}}, {key="-335" value=Number{value=334}}, {key="-336" value=String{value="v335"}}, {key="-337" value=Number{value=336}}, {key="-338" value=String{value="v337"}}, {key="-339" value=Number{value=338}}, {key="-340" value=String{value="v339"}}, {key="-341" value=Number{value=340}}, {key="-342" value=String{value="v341"}}, {key="-343" value=Number{value=342}}, {key="-344" value=String{value="v343"}}, {key="-345" value=Number{value=344}}, {key="-346" value=String{value="v345"}}, {key="-347" value=Number{value=346}}, {key="-348" value=String{value="v347"}}, {key="-349" value=Number{value=348}}, {key="-350" value=String{value="v349"}}, {key="-351" value=Number{value=350}}, {key="-352" value=String{value="v351"}}, {key="-353" value=Number{value=352}}, {key="-354" value=String{value="v353"}}, {key="-355" value=Number{value=354}}, {key="-356" value=String{value="v355"}}, {key="-357" value=Number{value=356}}, {key="-358" value=String{value="v357"}}, {key="-359" value=Number{value=358}}, {key="-360" value=String{value="v359"}}, {key="-361" value=Number{value=360}}, {key="-362" value=String{value="v361"}}, {key="-363" value=Number{value=362}}, {key="-364" value=String{value="v363"}}, {key="-365" value=Number{value=364}}, {key="-366" value=String{value="v365"}}, {key="-367" value=Number{value=366}}, {key="-368" value=String{value="v367"}}, {key="-369" value=Number{value=368}}, {key="-370" value=String{value="v369"}}, {key="-371" value=Number{value=370}}, {key="-372" value=String{value="v371"}}, {key="-373" value=Number{value=372}}, {key="-374" value=String{value="v373"}}, {key="-375" value=Number{value=374}}, {key="-376" value=String{value="v375"}}, {key="-377" value=Number{value=376}}, {key="-378" value=String{value="v377"}}, {key="-379" value=Number{value=378}}, {key="-380" value=String{value="v379"}}, {key="-381" value=Number{value=380}}, {key="-382" value=String{value="v381"}}, {key="-383" value=Number{value=382}}, {key="-384" value=String{value="v383"}}, {key="-385" value=Number{value=384}}, {key="-386" value=String{value="v385"}}, {key="-387" value=Number{value=386}}, {key="-388" value=String{value="v387"}}, {key="-389" value=Number{value=388}}, {key="-390" value=String{value="v389"}}, {key="-391" value=Number{value=390}}, {key="-392" value=String{value="v391"}}, {key="-393" value=Number{value=392}}, {key="-394" value=String{value="v393"}}, {key="-395" value=Number{value=394}}, {key="-396" value=String{value="v395"}}, {key="-397" value=Number{value=396}}, {key="-398" value=String{value="v397"}}, {key="-399" value=Number{value=398}}, {key="-400" value=String{value="v399"}}, {key="-401" value=Number{value=400}}, {key="-402" value=String{value="v401"}}, {key="-403" value=Number{value=402}}, {key="-404" value=String{value="v403"}}, {key="-405" value=Number{value=404}}, {key="-406" value=String{value="v405"}}, {key="-407" value=Number{value=406}}, {key="-408" value=String{value="v407"}}, {key="-409" value=Number{value=408}}, {key="-410" value=String{value="v409"}}, {key="-411" value=Number{value=410}}, {key="-412" value=String{value="v411"}}, {key="-413" value=Number{value=412}}, {key="-414" value=String{value="v413"}}, {key="-415" value=Number{value=414}}, {key="-416" value=String{value="v415"}}, {key="-417" value=Number{value=416}}, {key="-418" value=String{value="v417"}}, {key="-419" value=Number{value=418}}, {key="-420" value=String{value="v419"}}, {key="-421" value=Number{value=420}}, {key="-422" value=String{value="v421"}}, {key="-423" value=Number{value=422}}, {key="-424" value=String{value="v423"}}, {key="-425" value=Number{value=424}}, {key="-426" value=String{value="v425"}}, {key="-427" value=Number{value=426}}, {key="-428" value=String{value="v427"}}, {key="-429" value=Number{value=428}}, {key="-430" value=String{value="v429"}}, {key="-431" value=Number{value=430}}, {key="-432" value=String{value="v431"}}, {key="-433" value=Number{value=432}}, {key="-434" value=String{value="v433"}}, {key="-435" value=Number{value=434}}, {key="-436" value=String{value="v435"}}, {key="-437" value=Number{value=436}}, {key="-438" value=String{value="v437"}}, {key="-439" value=Number{value=438}}, {key="-440" value=String{value="v439"}}, {key="-441" value=Number{value=440}}, {key="-442" value=String{value="v441"}}, {key="-443" value=Number{value=442}}, {key="-444" value=String{value="v443"}}, {key="-445" value=Number{value=444}}, {key="-446" value=String{value="v445"}}, {key="-447" value=Number{value=446}}, {key="-448" value=String{value="v447"}}, {key="-449" value=Number{value=448}}, {key="-450" value=String{value="v449"}}, {key="-451" value=Number{value=450}}, {key="-452" value=String{value="v451"}}, {key="-453" value=Number{value=452}}, {key="-454" value=String{value="v453"}}, {key="-455" value=Number{value=454}}, {key="-456" value=String{value="v455"}}, {key="-457" value=Number{value=456}}, {key="-458" value=String{value="v457"}}, {key="-459" value=Number{value=458}}, {key="-460" value=String{value="v459"}}, {key="-461" value=Number{value=460}}, {key="-462" value=String{value="v461"}}, {key="-463" value=Number{value=462}}, {key="-464" value=String{value="v463"}}, {key="-465" value=Number{value=464}}, {key="-466" value=String{value="v465"}}, {key="-467" value=Number{value=466}}, {key="-468" value=String{value="v467"}}, {key="-469" value=Number{value=468}}, {key="-470" value=String{value="v469"}}, {key="-471" value=Number{value=470}}, {key="-472" value=String{value="v471"}}, {key="-473" value=Number{value=472}}, {key="-474" value=String{value="v473"}}, {key="-475" value=Number{value=474}}, {key="-476" value=String{value="v475"}}, {key="-477" value=Number{value=476}}, {key="-478" value=String{value="v477"}}, {key="-479" value=Number{value=478}}, {key="-480" value=String{value="v479"}}, {key="-481" value=Number{value=480}}, {key="-482" value=String{value="v481"}}, {key="-483" value=Number{value=482}}, {key="-484" value=String{value="v483"}}, {key="-485" value=Number{value=484}}, {key="-486" value=String{value="v485"}}, {key="-487" value=Number{value=486}}, {key="-488" value=String{value="v487"}}, {key="-489" value=Number{value=488}}, {key="-490" value=String{value="v489"}}, {key="-491" value=Number{value=490}}, {key="-492" value=String{value="v491"}}, {key="-493" value=Number{value=492}}, {key="-494" value=String{value="v493"}}, {key="-495" value=Number{value=494}}, {key="-496" value=String{value="v495"}}, {key="-497" value=Number{value=496}}, {key="-498" value=String{value="v497"}}, {key="-499" value=Number{value=498}}, {key="-500" value=String{value="v499"}}, {key="-501" value=Number{value=500}}, {key="-502" value=String{value="v501"}}, {key="-503" value=Number{value=502}}, {key="-504" value=String{value="v503"}}, {key="-505" value=Number{value=504}}, {key="-506" value=String{value="v505"}}, {key="-507" value=Number{value=506}}, {key="-508" value=String{value="v507"}}, {key="-509" value=Number{value=508}}, {key="-510" value=String{value="v509"}}, {key="-511" value=Number{value=510}}, {key="-512" value=String{value="v511"}}, {key="-513" value=Number{value=512}}, {key="-514" value=String{value="v513"}}]} module=m0
data d6 root=Object{layout=L4 members=[{key="first" value=Number{value=1}}, {key="3" value=String{value="three"}}, {key="m0" value=Number{value=0}}, {key="m1" value=Number{value=1}}, {key="m2" value=Number{value=2}}, {key="m3" value=Number{value=3}}, {key="m4" value=Number{value=4}}, {key="m5" value=Number{value=5}}, {key="m6" value=Number{value=6}}, {key="m7" value=Number{value=7}}, {key="m8" value=Number{value=8}}, {key="m9" value=Number{value=9}}, {key="m10" value=Number{value=10}}, {key="m11" value=Number{value=11}}, {key="m12" value=Number{value=12}}, {key="m13" value=Number{value=13}}, {key="m14" value=Number{value=14}}, {key="m15" value=Number{value=15}}]} module=m0
)IR"_s;

static constexpr ASCIILiteral describedTree = "{mixed:[1,hole,\"pear\",true,undefined,null,false],doubles:[1.5,hole,2,-0],integers:[1,2],inner:{zero:-0,nan:NaN,nothing:null},big:12345678901234567890n,text:\"plum\",nested:[[{zero:7,nan:8,nothing:undefined}],[]]}"_s;
static constexpr ASCIILiteral describedDoubles = "[0.5,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,-0]"_s;
static constexpr ASCIILiteral describedMixture = "[\"a0\",2,undefined,null,true,\"a5\",7,8,9,10,11,12,13,14,15,16,\"a16\"]"_s;

static constexpr ASCIILiteral describedDictionary = "{2:-0,10:\"ten\",name:\"fig\",size:3,content-type:\"text\",nested:{-1.5:[{deeper:12345678901234567890n}],-2.5:null},empty:{},w0:0,w1:1,w2:2,w3:3,w4:4,w5:5,w6:6,w7:7,w8:8,w9:9,w10:10,w11:11}"_s;
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

enum DataNumber : unsigned { Tree, Integers, Doubles, Mixture, Dictionary, LargeDictionary, SeenByUnknownCodeOnly, NumberOfData };

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
    AOT_TEST_CHECK(integers->indexingType() == ArrayWithInt32 && integers->length() == 2);
    AOT_TEST_CHECK(mixed->realm() == program.realm());
    AOT_TEST_CHECK(!mixed->canGetIndexQuickly(1u) && mixed->canGetIndexQuickly(4u) && mixed->getIndexQuickly(4).isUndefined());
    AOT_TEST_CHECK(!doubles->canGetIndexQuickly(1u) && doubles->canGetIndexQuickly(3u));
    AOT_TEST_CHECK(!isCopyOnWrite(mixed->indexingMode()) && !JSCellButterfly::isOutsideHeap(mixed->butterfly()));
    for (JSArray* array : { doubles, integers })
        AOT_TEST_CHECK(isCopyOnWrite(array->indexingMode()) && JSCellButterfly::isOutsideHeap(array->butterfly()));
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
    AOT_TEST_CHECK(nestedInFirst->extraProperties() && nestedInSecond->extraProperties());
    AOT_TEST_CHECK(nestedInFirst->extraProperties()->structure() == nestedInSecond->extraProperties()->structure());
    AOT_TEST_CHECK(!nestedInFirst->extraProperties()->structure()->isDictionary());

    callInPlainRealm(program, "(function (record) { delete record.w0; record['content-type'] = 'other'; record.added = 1; record[1] = 'one'; record.nested['-1.5'] = 0; record.w0 = 'again'; })"_s, { first });
    AOT_TEST_CHECK(isDescribedAs(program, first, "{1:\"one\",2:-0,10:\"ten\",name:\"fig\",size:3,content-type:\"other\",nested:{-1.5:0,-2.5:null},empty:{},w1:1,w2:2,w3:3,w4:4,w5:5,w6:6,w7:7,w8:8,w9:9,w10:10,w11:11,added:1,w0:\"again\"}"_s));
    AOT_TEST_CHECK(isDescribedAs(program, second, describedDictionary));
    AOT_TEST_CHECK(isDescribedAs(program, build(program, Dictionary), describedDictionary));
    AOT_TEST_CHECK(callInPlainRealm(program, "(function (record) { let d = Object.getOwnPropertyDescriptor(record, 'content-type'); return d.writable && d.enumerable && d.configurable && Object.isExtensible(record) && JSON.stringify(record.nested['-2.5']) === 'null'; })"_s, { second }).isTrue());
}

static void testManyMembersBecomeOneDictionary(LoadedProgram& program)
{
    StringBuilder expected;
    expected.append('{');
    for (unsigned index = 0; index < numberOfKeysInLargeDictionary; ++index) {
        if (index)
            expected.append(',');
        if (index % 2)
            expected.append('-', index + 1, ":\"v"_s, index, '"');
        else
            expected.append('-', index + 1, ':', index);
    }
    expected.append('}');
    String described = expected.toString();

    Object* first = compiledObject(build(program, LargeDictionary));
    Object* second = compiledObject(build(program, LargeDictionary));
    AOT_TEST_CHECK(first->extraProperties() && second->extraProperties());
    AOT_TEST_CHECK(first->extraProperties()->structure()->isDictionary() && first->extraProperties()->structure() != second->extraProperties()->structure());
    AOT_TEST_CHECK(describe(program, first) == described && describe(program, second) == described);
    AOT_TEST_CHECK(get(program, first, "-130"_s) == get(program, second, "-130"_s));

    callInPlainRealm(program, "(function (record) { for (let key of Object.keys(record)) delete record[key]; })"_s, { first });
    AOT_TEST_CHECK(isDescribedAs(program, first, "{}"_s));
    AOT_TEST_CHECK(describe(program, second) == described);
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

std::span<const NativeTest> dataBuilderTests()
{
    static constexpr NativeTest tests[] = {
        { "data-builder/nested-objects-and-arrays"_s, withData<testNestedObjectsAndArrays> },
        { "data-builder/members-that-are-no-fields"_s, withData<testMembersThatAreNoFields> },
        { "data-builder/many-members-become-one-dictionary"_s, withData<testManyMembersBecomeOneDictionary> },
        { collectionInTheMiddleTest, testCollectionInTheMiddleLosesNothing },
    };
    return tests;
}

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
