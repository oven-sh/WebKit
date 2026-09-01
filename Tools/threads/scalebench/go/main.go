// SCALEBENCH — Go implementation of Tools/threads/scalebench/SPEC.md.
//
// Concurrent in-memory inverted index over a synthetic corpus; three phases
// (INGEST / QUERY 90-10 / ANALYTICS) run back-to-back by W worker goroutines
// spawned once and reused across phases (hand-rolled counting barrier,
// sync.Mutex + sync.Cond, per SPEC §1).
//
// Fairness notes (SPEC §2):
//   - plain map[string]*posting + sync.Mutex per shard (NOT sync.Map, NOT RWMutex)
//   - atomic-claim counters as the work queue (NO channels for work distribution)
//   - sync/atomic used only for plain counters
//   - no floats anywhere in the measured program (timing excepted)
//
// GOMAXPROCS is left at its default per SPEC §2.7 ("Defaults only" + "Go one
// goroutine per worker with runtime.GOMAXPROCS left at default"): Go's
// scheduler and concurrent-GC workers are part of the platform, exactly as
// JSC's JIT/marking helper threads and the JVM's GC/JIT threads are.
//
// ALGORITHMIC FLOOR (SCALEBENCH.md §36 lens, 64-HW-thread host, NBASE=28000):
// `GOGC=off` W=16 — phaseA 202 ms, total 354 ms (2-rep stable 354/357). With
// the collector switched off this is the zero-GC, AOT-compiled, M:N-scheduled
// floor for the SPEC workload on this host: no JIT warmup, no write barrier,
// no mark assist. Go-with-GC W=16 phaseA ≈266 ms / total 410–457 ms, so Go's
// own GC costs ≈64 ms phaseA wall (perf shows ~20% CPU in gcBgMarkWorker but
// most of it is wall-free on idle Ps — `GOMAXPROCS=16` is FASTER at 380 ms
// than the default-64 410–457 ms, refuting any "idle-P free marking"
// advantage). Direct answer to the §36 "is there a JS-reachable ceiling?"
// lens: phaseA cannot go below ~202 ms and total cannot go below ~354 ms; at
// §36 JS-flat W=16 phaseA 570 ms there is 368 ms of removable headroom, and
// JS total 1053 ms = 3.0× floor while Java 925 ms = 2.6× floor — Java is ALSO
// far from the floor, so the residual JS→Java gap is not "JIT vs JIT".
//
// Run:  go run main.go <W>     (module-less; or: go build -o bench-go main.go)
// Env:  SCALEBENCH_SMOKE=1  => N_BASE=2000 (runner preflight, SPEC §5.1)
package main

import (
	"fmt"
	"os"
	"sort"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

// ---------------------------------------------------------------------------
// Constants (SPEC §1.1) — keep in sync with bench.js / Bench.java / run.sh.
// ---------------------------------------------------------------------------

const (
	GOLDEN     uint64 = 0x9E3779B97F4A7C15
	GLOBALSEED uint64 = 0x5CA1AB1E0BADF00D
	QUERYSEED  uint64 = 0xFACEFEEDC0FFEE11
	V          uint64 = 65536
	K          uint64 = 128
	WRITERMOD  uint64 = 10
	TOPN       int    = 20
	// N_BASE: pinned by SPEC §8 calibration, 2026-06-10 (Release jsc, W=1,
	// Phase A 14.7 s). Must match SPEC.md §1.1 / bench.js / Bench.java.
	NBASEDEFAULT uint64 = 28000
	NBASESMOKE   uint64 = 2000
)

var (
	nBase    uint64
	nQueries uint64
	workers  int
)

// ---------------------------------------------------------------------------
// PRNG — splitmix64 (SPEC §1.2)
// ---------------------------------------------------------------------------

type prng struct{ s uint64 }

func (p *prng) next() uint64 {
	p.s += GOLDEN
	z := p.s
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9
	z = (z ^ (z >> 27)) * 0x94D049BB133111EB
	return z ^ (z >> 31)
}

// mix: one stateless splitmix64 output step applied to x.
func mix(x uint64) uint64 {
	z := x + GOLDEN
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9
	z = (z ^ (z >> 27)) * 0x94D049BB133111EB
	return z ^ (z >> 31)
}

func prngSelfTest() {
	want := [3]uint64{0xe220a8397b1dcdaf, 0x6e789e6aa1b965f4, 0x06c45d188009454f}
	p := prng{s: 0}
	for i := 0; i < 3; i++ {
		if got := p.next(); got != want[i] {
			panic(fmt.Sprintf("splitmix64 self-test failed at %d: got %016x want %016x", i, got, want[i]))
		}
	}
}

// ---------------------------------------------------------------------------
// Hashing — FNV-1a 64 (SPEC §1.3)
// ---------------------------------------------------------------------------

func fnv1aString(s string) uint64 {
	h := uint64(0xCBF29CE484222325)
	for i := 0; i < len(s); i++ {
		h = (h ^ uint64(s[i])) * 0x100000001B3
	}
	return h
}

func fnv1aBytes(b []byte) uint64 {
	h := uint64(0xCBF29CE484222325)
	for _, c := range b {
		h = (h ^ uint64(c)) * 0x100000001B3
	}
	return h
}

// ---------------------------------------------------------------------------
// Vocabulary (SPEC §1.4)
// ---------------------------------------------------------------------------

// termString builds term i = "t" + base26(i) as a FRESH allocation on every
// use, exactly like bench.js termOf() and Bench.java termString(). A
// precomputed vocab table would be a one-language-only cache of shared data,
// banned by SPEC §2.2 — the per-pick string construction (CPU + allocation
// churn) is part of the measured workload in all three implementations.
func termString(i uint64) string {
	var buf [8]byte
	p := len(buf)
	for {
		p--
		buf[p] = byte('a' + i%26)
		i /= 26
		if i == 0 {
			break
		}
	}
	p--
	buf[p] = 't'
	return string(buf[p:])
}

// pickTerm: quadratic skew toward low indices (SPEC §1.4). Two PRNG draws.
func pickTerm(p *prng) uint64 {
	a := p.next() % V
	b := p.next() % V
	if a < b {
		return a
	}
	return b
}

// ---------------------------------------------------------------------------
// Document grammar (SPEC §1.5) + tokenizer (SPEC §1.6)
// ---------------------------------------------------------------------------

func genDocTokens(d uint64) []uint64 {
	p := prng{s: mix(GLOBALSEED ^ (d * GOLDEN))}
	titleLen := 5 + p.next()%8
	bodyLen := 80 + p.next()%121
	n := int(titleLen + bodyLen)
	toks := make([]uint64, n)
	for j := 0; j < n; j++ {
		toks[j] = pickTerm(&p)
	}
	return toks
}

func buildText(toks []uint64) string {
	var sb strings.Builder
	for j, ti := range toks {
		if j > 0 {
			sb.WriteByte(' ')
		}
		t := termString(ti)
		if j%7 == 0 {
			sb.WriteByte(t[0] - 32) // capitalize first letter (ASCII a-z)
			sb.WriteString(t[1:])
		} else {
			sb.WriteString(t)
		}
		if j%11 == 3 {
			sb.WriteByte(',')
		}
		if j%13 == 12 {
			sb.WriteByte('.')
		}
	}
	return sb.String()
}

// tokenize: split on any char not [a-zA-Z0-9], THEN lowercase each piece, per
// the SPEC §1.6 two-step sequence the other implementations follow: the raw
// split piece is allocated first (string(buf), like Java's piece.toString()
// and JS's text.substring), then a lowercased copy is allocated UNCONDITIONALLY
// (never skipped when already lowercase) — two allocations per token in all
// three languages.
func tokenize(text string) []string {
	var out []string
	buf := make([]byte, 0, 8) // scratch, analogous to Java's StringBuilder piece
	emit := func() {
		raw := string(buf) // split allocation
		lc := make([]byte, len(raw))
		for k := 0; k < len(raw); k++ {
			ch := raw[k]
			if ch >= 'A' && ch <= 'Z' {
				ch += 32
			}
			lc[k] = ch
		}
		out = append(out, string(lc)) // lowercase allocation (always)
		buf = buf[:0]
	}
	for i := 0; i < len(text); i++ {
		c := text[i]
		if c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' || c >= '0' && c <= '9' {
			buf = append(buf, c)
		} else if len(buf) > 0 {
			emit()
		}
	}
	if len(buf) > 0 {
		emit()
	}
	return out
}

// ---------------------------------------------------------------------------
// The shared index (SPEC §1.7)
// ---------------------------------------------------------------------------

// posting list: parallel arrays (docIds[], tfs[]) — mirror this shape in
// bench.js / Bench.java.
type posting struct {
	docIds []uint64
	tfs    []uint32
}

type shard struct {
	mu sync.Mutex
	m  map[string]*posting
}

// shards is a slice of POINTERS to individually allocated shard objects —
// the same abstraction level as Java's Shard[] (separately allocated
// ReentrantLock+HashMap objects) and JS's array of separately heap-allocated
// {lock, map} objects, and consistent with `groups map[string]*group` below.
// A value slice (`make([]shard, K)`) would pack four 16-byte shards (mutex +
// map header) per 64-byte cache line, so every lock CAS on a hot shard would
// also invalidate three neighbors' mutexes and map headers — a false-sharing
// contention amplifier that exists in no other implementation.
var shards []*shard

func initShards() {
	shards = make([]*shard, K)
	for i := range shards {
		shards[i] = &shard{m: make(map[string]*posting)}
	}
}

func shardFor(term string) *shard {
	return shards[fnv1aString(term)%K]
}

// Shared atomic counters (SPEC §1.7).
var (
	docsIngested    atomic.Uint64
	tokensProcessed atomic.Uint64
	queriesDone     atomic.Uint64
	writesDone      atomic.Uint64

	nextDoc   atomic.Uint64 // Phase A work queue
	nextOp    atomic.Uint64 // Phase B work queue
	nextShard atomic.Uint64 // Phase C work queue

	checksumBAcc atomic.Uint64 // per-thread Phase-B partials summed here
)

// df snapshot, frozen at the A/B barrier by worker 0; read-only thereafter.
var dfSnap map[string]uint64

// ---------------------------------------------------------------------------
// Barrier — hand-rolled counting barrier, Mutex+Cond (SPEC §1)
// ---------------------------------------------------------------------------

type barrier struct {
	mu    sync.Mutex
	cond  *sync.Cond
	n     int
	count int
	gen   uint64
}

func newBarrier(n int) *barrier {
	b := &barrier{n: n}
	b.cond = sync.NewCond(&b.mu)
	return b
}

func (b *barrier) await() {
	b.mu.Lock()
	g := b.gen
	b.count++
	if b.count == b.n {
		b.count = 0
		b.gen++
		b.cond.Broadcast()
	} else {
		for g == b.gen {
			b.cond.Wait()
		}
	}
	b.mu.Unlock()
}

// ---------------------------------------------------------------------------
// Ingest (Phase A body; also Phase B writers) — SPEC §1.8
// ---------------------------------------------------------------------------

func ingestDoc(d uint64) {
	toks := genDocTokens(d)
	text := buildText(toks)
	words := tokenize(text)
	tf := make(map[string]uint32)
	for _, w := range words {
		tf[w]++
	}
	// One lock acquisition per (doc, distinct-term) pair — no batching by shard.
	for term, c := range tf {
		sh := shardFor(term)
		sh.mu.Lock()
		pl := sh.m[term]
		if pl == nil {
			pl = &posting{}
			sh.m[term] = pl
		}
		pl.docIds = append(pl.docIds, d)
		pl.tfs = append(pl.tfs, c)
		sh.mu.Unlock()
	}
	tokensProcessed.Add(uint64(len(words)))
	docsIngested.Add(1)
}

func phaseAWork() {
	for {
		d := nextDoc.Add(1) - 1
		if d >= nBase {
			return
		}
		ingestDoc(d)
	}
}

// ---------------------------------------------------------------------------
// intcs arm (§39b full-workload 32-bit, supersedes the §39 checksum-only
// reroute): SAME work shape as the spec arm — string concat → tokenize →
// map[string].get/set under shard locks → 90/10 query mix → Phase C group-by
// — but EVERY numeric operation is uint32: splitmix32 PRNG, docSeedI/qSeedI =
// mix32(seed^d), pickTermI, shardForI = fnv1a32%K, mix32 everywhere, fnv1a32
// for all string hashing, uint32 per-goroutine partialB. Produces DIFFERENT
// documents than the spec u64 arm (different PRNG output) and so a NEW
// cross-language reference tuple — recorded in docs/threads/SCALEBENCH.md
// §39b. bench.js (Math.imul + >>>0) and Bench.java (int + >>>) implement
// byte-identical arithmetic. Default OFF: no `intcs` arg / SCALEBENCH_INTCS
// unset ⇒ the spec u64 path runs and the b3e65a68… reference is unchanged.
// ---------------------------------------------------------------------------

const (
	GSEED32  uint32 = 0x5ca1ab1e
	QSEED32  uint32 = 0xfacefeed
	GOLDEN32 uint32 = 0x9e3779b9
)

func fnv1a32(s string) uint32 {
	h := uint32(0x811c9dc5)
	for i := 0; i < len(s); i++ {
		h = (h ^ uint32(s[i])) * 0x01000193
	}
	return h
}

func mix32(x uint32) uint32 {
	z := x + 0x9e3779b9
	z = (z ^ (z >> 16)) * 0x85ebca6b
	z = (z ^ (z >> 13)) * 0xc2b2ae35
	return z ^ (z >> 16)
}

type prng32 struct{ s uint32 }

func (p *prng32) next() uint32 {
	p.s += GOLDEN32
	z := p.s
	z = (z ^ (z >> 16)) * 0x85ebca6b
	z = (z ^ (z >> 13)) * 0xc2b2ae35
	return z ^ (z >> 16)
}

func docSeedI(d uint64) uint32 { return mix32(GSEED32 ^ uint32(d)) }
func qSeedI(q uint64) uint32   { return mix32(QSEED32 ^ uint32(q)) }
func shardForI(term string) *shard {
	return shards[fnv1a32(term)%uint32(K)]
}
func pickTermI(p *prng32) uint64 {
	a := p.next() & uint32(V-1)
	b := p.next() & uint32(V-1)
	if a < b {
		return uint64(a)
	}
	return uint64(b)
}
func fnvU32LE(h, x uint32) uint32 {
	for i := 0; i < 4; i++ {
		h = (h ^ (x & 0xff)) * 0x01000193
		x >>= 8
	}
	return h
}

func genDocTokensI(d uint64) []uint64 {
	p := prng32{s: docSeedI(d)}
	titleLen := 5 + p.next()%8
	bodyLen := 80 + p.next()%121
	n := int(titleLen + bodyLen)
	toks := make([]uint64, n)
	for j := 0; j < n; j++ {
		toks[j] = pickTermI(&p)
	}
	return toks
}

func ingestDocI(d uint64) {
	toks := genDocTokensI(d)
	text := buildText(toks)
	words := tokenize(text)
	tf := make(map[string]uint32)
	for _, w := range words {
		tf[w]++
	}
	for term, c := range tf {
		sh := shardForI(term)
		sh.mu.Lock()
		pl := sh.m[term]
		if pl == nil {
			pl = &posting{}
			sh.m[term] = pl
		}
		pl.docIds = append(pl.docIds, d)
		pl.tfs = append(pl.tfs, c)
		sh.mu.Unlock()
	}
	tokensProcessed.Add(uint64(len(words)))
	docsIngested.Add(1)
}

func phaseAWorkI() {
	for {
		d := nextDoc.Add(1) - 1
		if d >= nBase {
			return
		}
		ingestDocI(d)
	}
}

func indexChecksum32() (sum uint64, count uint64) {
	var s uint32
	for k := range shards {
		for term, pl := range shards[k].m {
			th := fnv1a32(term)
			for i, d := range pl.docIds {
				item := th ^ (uint32(d) * 0xd6e8feb9) ^ (pl.tfs[i] * 0xcaaf00dd)
				s += mix32(item)
				count++
			}
		}
	}
	sum = uint64(s)
	return
}

func copyPostingsI(term string) ([]uint64, []uint32) {
	sh := shardForI(term)
	sh.mu.Lock()
	pl := sh.m[term]
	var ids []uint64
	var tfs []uint32
	if pl != nil {
		ids = append([]uint64(nil), pl.docIds...)
		tfs = append([]uint32(nil), pl.tfs...)
	}
	sh.mu.Unlock()
	return ids, tfs
}

func pointQueryI(p *prng32) uint32 {
	term := termString(pickTermI(p))
	sh := shardForI(term)
	var df, sumTf uint32
	sh.mu.Lock()
	if pl := sh.m[term]; pl != nil {
		for i, d := range pl.docIds {
			if d < nBase {
				df++
				sumTf += pl.tfs[i]
			}
		}
	}
	sh.mu.Unlock()
	return mix32(fnv1a32(term) ^ (df * 0x9e37) ^ sumTf)
}

func andQueryI(p *prng32) uint32 {
	nTerms := int(2 + p.next()%2)
	terms := make([]string, nTerms)
	for i := range terms {
		terms[i] = termString(pickTermI(p))
	}
	lists := make([][]uint64, nTerms)
	for i, t := range terms {
		lists[i], _ = copyPostingsI(t)
	}
	m := make(map[uint64]int)
	for _, d := range lists[0] {
		if d < nBase {
			m[d] = 1
		}
	}
	for i := 1; i < nTerms; i++ {
		for _, d := range lists[i] {
			if d < nBase && m[d] == i {
				m[d] = i + 1
			}
		}
	}
	var sum, count uint32
	for d, c := range m {
		if c == nTerms {
			sum += mix32(uint32(d))
			count++
		}
	}
	return mix32(sum) ^ count
}

func scoredQueryI(p *prng32) uint32 {
	nTerms := int(2 + p.next()%2)
	score := make(map[uint64]uint32)
	for i := 0; i < nTerms; i++ {
		term := termString(pickTermI(p))
		ids, tfs := copyPostingsI(term)
		var idf uint32
		if df := dfSnap[term]; df != 0 {
			idf = uint32(nBase) * 1000 / uint32(df)
		}
		for j, d := range ids {
			if d < nBase {
				score[d] += tfs[j] * idf
			}
		}
	}
	type cand struct {
		d uint64
		s uint32
	}
	cands := make([]cand, 0, len(score))
	for d, s := range score {
		cands = append(cands, cand{d, s})
	}
	sort.Slice(cands, func(i, j int) bool {
		if cands[i].s != cands[j].s {
			return cands[i].s > cands[j].s
		}
		return cands[i].d < cands[j].d
	})
	if len(cands) > TOPN {
		cands = cands[:TOPN]
	}
	h := uint32(0x811c9dc5)
	for _, c := range cands {
		h = fnvU32LE(h, uint32(c.d))
	}
	for _, c := range cands {
		h = fnvU32LE(h, c.s)
	}
	return h
}

func phaseBWorkI() {
	var local uint32
	for {
		q := nextOp.Add(1) - 1
		if q >= nQueries {
			break
		}
		if q%WRITERMOD == 0 {
			ingestDocI(nBase + q/WRITERMOD)
			writesDone.Add(1)
			continue
		}
		p := prng32{s: qSeedI(q)}
		kind := p.next() % 10
		var h uint32
		switch {
		case kind < 4:
			h = pointQueryI(&p)
		case kind < 8:
			h = andQueryI(&p)
		default:
			h = scoredQueryI(&p)
		}
		local += mix32((uint32(q) * GOLDEN32) ^ h)
		queriesDone.Add(1)
	}
	checksumBAcc.Add(uint64(local))
}

func finalizeCI() uint32 {
	var sum uint32
	for _, key := range groupKeys {
		g := groups[key]
		sort.Slice(g.terms, func(i, j int) bool {
			if g.terms[i].totalTf != g.terms[j].totalTf {
				return g.terms[i].totalTf > g.terms[j].totalTf
			}
			return g.terms[i].term < g.terms[j].term
		})
		n := len(g.terms)
		if n > TOPN {
			n = TOPN
		}
		var sb strings.Builder
		for i := 0; i < n; i++ {
			sb.WriteString(g.terms[i].term)
			sb.WriteByte(':')
			sb.WriteString(strconv.FormatUint(g.terms[i].totalTf, 10))
			sb.WriteByte(',')
		}
		sum += mix32(fnv1a32(key) ^ uint32(g.totalTf) ^ (uint32(g.df) * GOLDEN32) ^ fnv1a32(sb.String()))
	}
	return sum
}

// ---------------------------------------------------------------------------
// §40 NOCONCAT ARM — intcs base with the buildText→tokenize round-trip
// removed: genDocTermsI emits the term strings directly. Everything
// downstream (tf map[string], shardForI, shard.m get/put, queries, phaseC)
// is byte-identical to intcs.
// ---------------------------------------------------------------------------

func genDocTermsI(d uint64) []string {
	p := prng32{s: docSeedI(d)}
	titleLen := 5 + p.next()%8
	bodyLen := 80 + p.next()%121
	n := int(titleLen + bodyLen)
	toks := make([]string, n)
	for j := 0; j < n; j++ {
		toks[j] = termString(pickTermI(&p))
	}
	return toks
}

func ingestDocNC(d uint64) {
	words := genDocTermsI(d)
	tf := make(map[string]uint32)
	for _, w := range words {
		tf[w]++
	}
	for term, c := range tf {
		sh := shardForI(term)
		sh.mu.Lock()
		pl := sh.m[term]
		if pl == nil {
			pl = &posting{}
			sh.m[term] = pl
		}
		pl.docIds = append(pl.docIds, d)
		pl.tfs = append(pl.tfs, c)
		sh.mu.Unlock()
	}
	tokensProcessed.Add(uint64(len(words)))
	docsIngested.Add(1)
}

func phaseAWorkNC() {
	for {
		d := nextDoc.Add(1) - 1
		if d >= nBase {
			return
		}
		ingestDocNC(d)
	}
}

func phaseBWorkNC() {
	var local uint32
	for {
		q := nextOp.Add(1) - 1
		if q >= nQueries {
			break
		}
		if q%WRITERMOD == 0 {
			ingestDocNC(nBase + q/WRITERMOD)
			writesDone.Add(1)
			continue
		}
		p := prng32{s: qSeedI(q)}
		kind := p.next() % 10
		var h uint32
		switch {
		case kind < 4:
			h = pointQueryI(&p)
		case kind < 8:
			h = andQueryI(&p)
		default:
			h = scoredQueryI(&p)
		}
		local += mix32((uint32(q) * GOLDEN32) ^ h)
		queriesDone.Add(1)
	}
	checksumBAcc.Add(uint64(local))
}

// ---------------------------------------------------------------------------
// §40 NOMAP ARM — intcs base with every map[string] lookup replaced by a
// direct integer index. Full string round-trip (buildText→tokenize) KEPT.
// ---------------------------------------------------------------------------

// invTermOf: base26-decode the lowercased token back to its termId.
func invTermOf(s string) int {
	r := 0
	for i := 1; i < len(s); i++ {
		r = r*26 + int(s[i]-'a')
	}
	return r
}

func firstLetterIdx(t int) int {
	for t >= 26 {
		t /= 26
	}
	return t
}

func termLenBucket(t int) int {
	switch {
	case t < 26:
		return 2
	case t < 676:
		return 3
	case t < 17576:
		return 4
	default:
		return 5
	}
}

var (
	nmShardOf []int32   // nmShardOf[t] = fnv1a32(termString(t)) % K
	nmPost    []posting // V slots; nmPost[t].docIds==nil means empty
	nmDfSnap  []int32
)

type nmLocal struct {
	tf    [V]int32
	dirty [256]int32
}

func initNM() {
	nmShardOf = make([]int32, V)
	for t := 0; t < int(V); t++ {
		nmShardOf[t] = int32(fnv1a32(termString(uint64(t))) % uint32(K))
	}
	nmPost = make([]posting, V)
}

func ingestDocNM(d uint64, sc *nmLocal) {
	toks := genDocTokensI(d)
	text := buildText(toks)
	words := tokenize(text)
	nDirty := 0
	for _, w := range words {
		t := invTermOf(w)
		if sc.tf[t] == 0 {
			sc.dirty[nDirty] = int32(t)
			nDirty++
		}
		sc.tf[t]++
	}
	for k := 0; k < nDirty; k++ {
		t := sc.dirty[k]
		c := sc.tf[t]
		sc.tf[t] = 0
		sh := shards[nmShardOf[t]]
		sh.mu.Lock()
		pl := &nmPost[t]
		pl.docIds = append(pl.docIds, d)
		pl.tfs = append(pl.tfs, uint32(c))
		sh.mu.Unlock()
	}
	tokensProcessed.Add(uint64(len(words)))
	docsIngested.Add(1)
}

func phaseAWorkNM(sc *nmLocal) {
	for {
		d := nextDoc.Add(1) - 1
		if d >= nBase {
			return
		}
		ingestDocNM(d, sc)
	}
}

func indexChecksumNM() (sum uint64, count uint64) {
	var s uint32
	for t := 0; t < int(V); t++ {
		pl := &nmPost[t]
		if len(pl.docIds) == 0 {
			continue
		}
		th := mix32(uint32(t))
		for i, d := range pl.docIds {
			item := th ^ (uint32(d) * 0xd6e8feb9) ^ (pl.tfs[i] * 0xcaaf00dd)
			s += mix32(item)
			count++
		}
	}
	sum = uint64(s)
	return
}

func buildDfSnapNM() {
	nmDfSnap = make([]int32, V)
	for t := 0; t < int(V); t++ {
		nmDfSnap[t] = int32(len(nmPost[t].docIds))
	}
}

func copyPostingsNM(t int) ([]uint64, []uint32) {
	sh := shards[nmShardOf[t]]
	sh.mu.Lock()
	pl := &nmPost[t]
	var ids []uint64
	var tfs []uint32
	if len(pl.docIds) > 0 {
		ids = append([]uint64(nil), pl.docIds...)
		tfs = append([]uint32(nil), pl.tfs...)
	}
	sh.mu.Unlock()
	return ids, tfs
}

func pointQueryNM(p *prng32) uint32 {
	t := int(pickTermI(p))
	sh := shards[nmShardOf[t]]
	var df, sumTf uint32
	sh.mu.Lock()
	pl := &nmPost[t]
	for i, d := range pl.docIds {
		if d < nBase {
			df++
			sumTf += pl.tfs[i]
		}
	}
	sh.mu.Unlock()
	return mix32(mix32(uint32(t)) ^ (df * 0x9e37) ^ sumTf)
}

func andQueryNM(p *prng32) uint32 {
	nTerms := int(2 + p.next()%2)
	lists := make([][]uint64, nTerms)
	for i := range lists {
		lists[i], _ = copyPostingsNM(int(pickTermI(p)))
	}
	m := make(map[uint64]int)
	for _, d := range lists[0] {
		if d < nBase {
			m[d] = 1
		}
	}
	for i := 1; i < nTerms; i++ {
		for _, d := range lists[i] {
			if d < nBase && m[d] == i {
				m[d] = i + 1
			}
		}
	}
	var sum, count uint32
	for d, c := range m {
		if c == nTerms {
			sum += mix32(uint32(d))
			count++
		}
	}
	return mix32(sum) ^ count
}

func scoredQueryNM(p *prng32) uint32 {
	nTerms := int(2 + p.next()%2)
	score := make(map[uint64]uint32)
	for i := 0; i < nTerms; i++ {
		t := int(pickTermI(p))
		ids, tfs := copyPostingsNM(t)
		var idf uint32
		if df := nmDfSnap[t]; df != 0 {
			idf = uint32(nBase) * 1000 / uint32(df)
		}
		for j, d := range ids {
			if d < nBase {
				score[d] += tfs[j] * idf
			}
		}
	}
	type cand struct {
		d uint64
		s uint32
	}
	cands := make([]cand, 0, len(score))
	for d, s := range score {
		cands = append(cands, cand{d, s})
	}
	sort.Slice(cands, func(i, j int) bool {
		if cands[i].s != cands[j].s {
			return cands[i].s > cands[j].s
		}
		return cands[i].d < cands[j].d
	})
	if len(cands) > TOPN {
		cands = cands[:TOPN]
	}
	h := uint32(0x811c9dc5)
	for _, c := range cands {
		h = fnvU32LE(h, uint32(c.d))
	}
	for _, c := range cands {
		h = fnvU32LE(h, c.s)
	}
	return h
}

func phaseBWorkNM(sc *nmLocal) {
	var local uint32
	for {
		q := nextOp.Add(1) - 1
		if q >= nQueries {
			break
		}
		if q%WRITERMOD == 0 {
			ingestDocNM(nBase+q/WRITERMOD, sc)
			writesDone.Add(1)
			continue
		}
		p := prng32{s: qSeedI(q)}
		kind := p.next() % 10
		var h uint32
		switch {
		case kind < 4:
			h = pointQueryNM(&p)
		case kind < 8:
			h = andQueryNM(&p)
		default:
			h = scoredQueryNM(&p)
		}
		local += mix32((uint32(q) * GOLDEN32) ^ h)
		queriesDone.Add(1)
	}
	checksumBAcc.Add(uint64(local))
}

type nmGroup struct {
	mu      sync.Mutex
	totalTf uint64
	df      uint64
	termIds []int
	termTfs []uint64
}

var nmGroups []*nmGroup

func initNMGroups() {
	nmGroups = make([]*nmGroup, 104)
	for i := range nmGroups {
		nmGroups[i] = &nmGroup{}
	}
}

func phaseCWorkNM() {
	block := int(V) / int(K) // 512
	for {
		s := nextShard.Add(1) - 1
		if s >= K {
			return
		}
		lo := int(s) * block
		hi := lo + block
		for t := lo; t < hi; t++ {
			pl := &nmPost[t]
			df := len(pl.docIds)
			if df == 0 {
				continue
			}
			var tot uint64
			for _, tf := range pl.tfs {
				tot += uint64(tf)
			}
			gk := (termLenBucket(t)-2)*26 + firstLetterIdx(t)
			g := nmGroups[gk]
			g.mu.Lock()
			g.totalTf += tot
			g.df += uint64(df)
			g.termIds = append(g.termIds, t)
			g.termTfs = append(g.termTfs, tot)
			g.mu.Unlock()
		}
	}
}

func finalizeCNM() uint32 {
	var sum uint32
	for gk := 0; gk < 104; gk++ {
		g := nmGroups[gk]
		n := len(g.termIds)
		idx := make([]int, n)
		for i := range idx {
			idx[i] = i
		}
		sort.Slice(idx, func(a, b int) bool {
			if g.termTfs[idx[a]] != g.termTfs[idx[b]] {
				return g.termTfs[idx[a]] > g.termTfs[idx[b]]
			}
			return g.termIds[idx[a]] < g.termIds[idx[b]]
		})
		top := n
		if top > TOPN {
			top = TOPN
		}
		var sb strings.Builder
		for i := 0; i < top; i++ {
			sb.WriteString(termString(uint64(g.termIds[idx[i]])))
			sb.WriteByte(':')
			sb.WriteString(strconv.FormatUint(g.termTfs[idx[i]], 10))
			sb.WriteByte(',')
		}
		l := gk/26 + 2
		key := string([]byte{byte('0' + l), ':', byte('a' + gk%26)})
		sum += mix32(fnv1a32(key) ^ uint32(g.totalTf) ^ (uint32(g.df) * GOLDEN32) ^ fnv1a32(sb.String()))
	}
	return sum
}

// indexChecksum: order-independent mod-2^64 sum over every posting (SPEC §1.8).
func indexChecksum() (sum uint64, count uint64) {
	for k := range shards {
		for term, pl := range shards[k].m {
			th := fnv1aString(term)
			for i, d := range pl.docIds {
				sum += mix(th ^ (d * 0xD6E8FEB86659FD93) ^ (uint64(pl.tfs[i]) * 0xCAAF00DD))
				count++
			}
		}
	}
	return
}

func buildDfSnap() {
	dfSnap = make(map[string]uint64, 1<<16)
	for k := range shards {
		for term, pl := range shards[k].m {
			dfSnap[term] = uint64(len(pl.docIds))
		}
	}
}

// ---------------------------------------------------------------------------
// Phase B — QUERY, 90% read / 10% write (SPEC §1.9)
// ---------------------------------------------------------------------------

// copyPostings: snapshot a term's posting list under its shard lock.
func copyPostings(term string) ([]uint64, []uint32) {
	sh := shardFor(term)
	sh.mu.Lock()
	pl := sh.m[term]
	var ids []uint64
	var tfs []uint32
	if pl != nil {
		ids = append([]uint64(nil), pl.docIds...)
		tfs = append([]uint32(nil), pl.tfs...)
	}
	sh.mu.Unlock()
	return ids, tfs
}

func pointQuery(p *prng) uint64 {
	term := termString(pickTerm(p))
	sh := shardFor(term)
	var df, sumTf uint64
	sh.mu.Lock()
	if pl := sh.m[term]; pl != nil {
		for i, d := range pl.docIds {
			if d < nBase { // base postings only — writer-independent
				df++
				sumTf += uint64(pl.tfs[i])
			}
		}
	}
	sh.mu.Unlock()
	return mix(fnv1aString(term) ^ (df * 0x9E37) ^ sumTf)
}

func andQuery(p *prng) uint64 {
	nTerms := int(2 + p.next()%2)
	terms := make([]string, nTerms)
	for i := range terms {
		terms[i] = termString(pickTerm(p)) // duplicates allowed; keep them
	}
	lists := make([][]uint64, nTerms)
	for i, t := range terms {
		lists[i], _ = copyPostings(t)
	}
	m := make(map[uint64]int)
	for _, d := range lists[0] {
		if d < nBase {
			m[d] = 1
		}
	}
	for i := 1; i < nTerms; i++ {
		for _, d := range lists[i] {
			if d < nBase && m[d] == i {
				m[d] = i + 1
			}
		}
	}
	var sum, count uint64
	for d, c := range m {
		if c == nTerms {
			sum += mix(d)
			count++
		}
	}
	return mix(sum) ^ count
}

func appendLE64(b []byte, v uint64) []byte {
	return append(b, byte(v), byte(v>>8), byte(v>>16), byte(v>>24),
		byte(v>>32), byte(v>>40), byte(v>>48), byte(v>>56))
}

func scoredQuery(p *prng) uint64 {
	nTerms := int(2 + p.next()%2)
	score := make(map[uint64]uint64) // union of candidates, base docs only
	for i := 0; i < nTerms; i++ {
		term := termString(pickTerm(p))
		ids, tfs := copyPostings(term)
		var idf uint64
		if df := dfSnap[term]; df != 0 {
			idf = nBase * 1000 / df // u64 integer division; dfSnap miss => 0
		}
		for j, d := range ids {
			if d < nBase {
				score[d] += uint64(tfs[j]) * idf // creates entry even at idf=0 (union)
			}
		}
	}
	type cand struct{ d, s uint64 }
	cands := make([]cand, 0, len(score))
	for d, s := range score {
		cands = append(cands, cand{d, s})
	}
	sort.Slice(cands, func(i, j int) bool { // score DESC, docId ASC
		if cands[i].s != cands[j].s {
			return cands[i].s > cands[j].s
		}
		return cands[i].d < cands[j].d
	})
	if len(cands) > TOPN {
		cands = cands[:TOPN]
	}
	buf := make([]byte, 0, len(cands)*16)
	for _, c := range cands {
		buf = appendLE64(buf, c.d)
	}
	for _, c := range cands {
		buf = appendLE64(buf, c.s)
	}
	return fnv1aBytes(buf)
}

func phaseBWork() {
	var local uint64
	for {
		q := nextOp.Add(1) - 1
		if q >= nQueries {
			break
		}
		if q%WRITERMOD == 0 { // writer
			ingestDoc(nBase + q/WRITERMOD)
			writesDone.Add(1)
			continue
		}
		p := prng{s: mix(QUERYSEED ^ (q * GOLDEN))}
		kind := p.next() % 10
		var h uint64
		switch {
		case kind < 4:
			h = pointQuery(&p)
		case kind < 8:
			h = andQuery(&p)
		default:
			h = scoredQuery(&p)
		}
		local += mix((q * GOLDEN) ^ h)
		queriesDone.Add(1)
	}
	checksumBAcc.Add(local)
}

// ---------------------------------------------------------------------------
// Phase C — ANALYTICS (SPEC §1.10)
// ---------------------------------------------------------------------------

type termStat struct {
	term    string
	totalTf uint64
}

type group struct {
	mu      sync.Mutex
	totalTf uint64
	df      uint64
	terms   []termStat
}

var (
	groups    map[string]*group
	groupKeys []string // canonical order for the (order-independent) checksum
)

// Pre-populate all 104 keys (len 2..5 x first letter a..z) before Phase C so
// the outer map needs no lock (SPEC §1.10).
func initGroups() {
	groups = make(map[string]*group, 104)
	groupKeys = make([]string, 0, 104)
	for l := 2; l <= 5; l++ {
		for c := byte('a'); c <= 'z'; c++ {
			key := string([]byte{byte('0' + l), ':', c})
			groups[key] = &group{}
			groupKeys = append(groupKeys, key)
		}
	}
}

// groupKey: string length crossed with the first letter of the BASE26 part,
// i.e. term[1] — every term starts with the literal 't', so term[0] would
// collapse the 26-letter cross to one letter (4 groups instead of up to 104)
// and break checksumC against bench.js/Bench.java, which both use charAt(1).
func groupKey(term string) string {
	return string([]byte{byte('0' + len(term)), ':', term[1]})
}

func phaseCWork() {
	for {
		s := nextShard.Add(1) - 1
		if s >= K {
			return
		}
		sh := shards[s]
		// Index is quiescent after the B/C barrier; shard iteration needs no lock.
		for term, pl := range sh.m {
			var tot uint64
			for _, tf := range pl.tfs {
				tot += uint64(tf)
			}
			df := uint64(len(pl.docIds)) // ALL postings: base + writer docs
			g := groups[groupKey(term)]
			g.mu.Lock()
			g.totalTf += tot
			g.df += df
			g.terms = append(g.terms, termStat{term, tot})
			g.mu.Unlock()
		}
	}
}

// finalizeC: worker 0, after the final barrier — sort each group's terms by
// (totalTf DESC, term ASC), take top TOPN, fold into checksumC.
func finalizeC() uint64 {
	var sum uint64
	for _, key := range groupKeys { // all 104 groups, empty ones included
		g := groups[key]
		sort.Slice(g.terms, func(i, j int) bool {
			if g.terms[i].totalTf != g.terms[j].totalTf {
				return g.terms[i].totalTf > g.terms[j].totalTf
			}
			return g.terms[i].term < g.terms[j].term
		})
		n := len(g.terms)
		if n > TOPN {
			n = TOPN
		}
		var sb strings.Builder
		for i := 0; i < n; i++ { // join as "term:totalTf," (trailing comma per entry)
			sb.WriteString(g.terms[i].term)
			sb.WriteByte(':')
			sb.WriteString(strconv.FormatUint(g.terms[i].totalTf, 10))
			sb.WriteByte(',')
		}
		sum += mix(fnv1aString(key) ^ g.totalTf ^ (g.df * GOLDEN) ^ fnv1aString(sb.String()))
	}
	return sum
}

// ---------------------------------------------------------------------------
// Phase C — WORK-STEALING ARM (SPEC §1.10-WS; mode-selected, env
// SCALEBENCH_WS=1; the naive phaseCWork above is untouched).
//
// Identical algorithm to bench.js phaseCWS / Bench.java phaseCWS:
//   - W per-worker deques over the 128 shard indices; shard s seeded into
//     deque s % W in increasing s order. Fixed array, [head, tail) live
//     region, plain sync.Mutex per deque (the one mutex type, §2.1 — no
//     lock-free Chase-Lev in any language).
//   - Owner pops from the TAIL; when empty, scan victims (w+1..w+W-1 mod W)
//     and steal ceil(n/2) from the victim's HEAD (victim lock for the copy,
//     then own lock to append — never nested).
//   - Shared atomic wsRemaining (init K) decremented per popped shard;
//     a worker exits when own deque empty + full scan failed + remaining==0.
//   - THREAD-LOCAL accumulation (map groupKey -> *wsLocalGroup, no group
//     locks); worker 0 merges all W local maps single-threaded after the
//     Phase C barrier, then the naive finalizeC() runs unchanged.
// ---------------------------------------------------------------------------

type wsDeque struct {
	mu         sync.Mutex
	arr        []uint64
	head, tail int
}

type wsLocalGroup struct {
	totalTf uint64
	df      uint64
	terms   []termStat
}

var (
	wsMode      bool
	intcs       bool
	noconcat    bool
	nomap       bool
	wsDeques    []*wsDeque
	wsLocals    []map[string]*wsLocalGroup
	wsRemaining atomic.Int64
)

func initWS() {
	wsDeques = make([]*wsDeque, workers)
	for w := 0; w < workers; w++ {
		wsDeques[w] = &wsDeque{}
	}
	for s := uint64(0); s < K; s++ {
		dq := wsDeques[int(s)%workers]
		dq.arr = append(dq.arr, s)
		dq.tail++
	}
	wsLocals = make([]map[string]*wsLocalGroup, workers)
	wsRemaining.Store(int64(K))
}

func wsPopOwn(w int) (uint64, bool) {
	dq := wsDeques[w]
	dq.mu.Lock()
	if dq.head < dq.tail {
		dq.tail--
		item := dq.arr[dq.tail]
		dq.mu.Unlock()
		return item, true
	}
	dq.mu.Unlock()
	return 0, false
}

func wsSteal(w int) bool {
	for off := 1; off < workers; off++ {
		dq := wsDeques[(w+off)%workers]
		var stolen []uint64
		dq.mu.Lock()
		if n := dq.tail - dq.head; n > 0 {
			cnt := (n + 1) / 2 // steal-half = ceil(n/2), from the head
			stolen = append(stolen, dq.arr[dq.head:dq.head+cnt]...)
			dq.head += cnt
		}
		dq.mu.Unlock()
		if stolen != nil {
			own := wsDeques[w]
			own.mu.Lock()
			own.arr = append(own.arr[:own.tail], stolen...)
			own.tail += len(stolen)
			own.mu.Unlock()
			return true
		}
	}
	return false
}

func phaseCWSWork(id int) {
	local := make(map[string]*wsLocalGroup) // thread-local accumulator
	for {
		s, ok := wsPopOwn(id)
		if !ok {
			if wsSteal(id) {
				continue
			}
			if wsRemaining.Load() <= 0 {
				break
			}
			continue // transiently in-flight steal; re-scan
		}
		wsRemaining.Add(-1)
		sh := shards[s]
		for term, pl := range sh.m {
			var tot uint64
			for _, tf := range pl.tfs {
				tot += uint64(tf)
			}
			df := uint64(len(pl.docIds)) // ALL postings: base + writer docs
			key := groupKey(term)
			g := local[key]
			if g == nil {
				g = &wsLocalGroup{}
				local[key] = g
			}
			g.totalTf += tot
			g.df += df
			g.terms = append(g.terms, termStat{term, tot})
		}
	}
	wsLocals[id] = local
}

// wsMergeLocals: worker 0, single-threaded after the Phase C barrier — merge
// the W thread-local accumulators into the shared groups; finalizeC() then
// runs unchanged.
func wsMergeLocals() {
	for w := 0; w < workers; w++ {
		for key, lg := range wsLocals[w] {
			g := groups[key]
			g.totalTf += lg.totalTf
			g.df += lg.df
			g.terms = append(g.terms, lg.terms...)
		}
	}
}

// ---------------------------------------------------------------------------
// Worker + main
// ---------------------------------------------------------------------------

var (
	bar *barrier

	// Written only by worker 0; read by main after join (channel receive
	// provides the happens-before edge).
	checksumA, checksumA2, checksumB, checksumC, postingsCount uint64
	phaseAms, phaseBms, phaseCms, totalms                      float64

	tTotal0, tPhase time.Time
)

// msSince: fractional milliseconds (timing is explicitly exempt from the
// SPEC §2.5 no-float rule). All three implementations report sub-ms phase
// times: integer-ms truncation would floor-bias high-W Phase B/C cells
// (single-digit ms at W>=32) by up to 1 ms — for Go/Java only — exactly in
// the saturation-knee region the matrix exists to measure.
func msSince(t time.Time) float64 {
	return float64(time.Since(t).Nanoseconds()) / 1e6
}

func worker(id int, done chan<- struct{}) {
	var nmSc *nmLocal
	if nomap {
		nmSc = &nmLocal{}
	}
	bar.await() // all workers spawned; clock starts
	if id == 0 {
		tTotal0 = time.Now()
		tPhase = tTotal0
	}

	switch {
	case nomap:
		phaseAWorkNM(nmSc)
	case noconcat:
		phaseAWorkNC()
	case intcs:
		phaseAWorkI()
	default:
		phaseAWork()
	}
	bar.await() // Phase A done
	if id == 0 {
		phaseAms = msSince(tPhase)
		switch {
		case nomap:
			checksumA, postingsCount = indexChecksumNM()
			buildDfSnapNM()
		case noconcat, intcs:
			checksumA, postingsCount = indexChecksum32()
			buildDfSnap()
		default:
			checksumA, postingsCount = indexChecksum() // after the barrier, thread 0
			buildDfSnap()                              // frozen df snapshot, published before B
		}
		tPhase = time.Now()
	}
	bar.await() // dfSnap published; Phase B starts

	switch {
	case nomap:
		phaseBWorkNM(nmSc)
	case noconcat:
		phaseBWorkNC()
	case intcs:
		phaseBWorkI()
	default:
		phaseBWork()
	}
	bar.await() // Phase B done
	if id == 0 {
		phaseBms = msSince(tPhase)
		checksumB = checksumBAcc.Load()
		switch {
		case nomap:
			checksumB &= 0xFFFFFFFF
			checksumA2, _ = indexChecksumNM()
		case noconcat, intcs:
			checksumB &= 0xFFFFFFFF // §39b: per-goroutine uint32 partials, summed mod-2^32
			checksumA2, _ = indexChecksum32()
		default:
			checksumA2, _ = indexChecksum() // base + writer docs
		}
		tPhase = time.Now()
	}
	bar.await() // Phase C starts

	switch {
	case nomap:
		phaseCWorkNM()
	case wsMode:
		phaseCWSWork(id)
	default:
		phaseCWork()
	}
	bar.await() // Phase C done
	if id == 0 {
		phaseCms = msSince(tPhase)
		if wsMode && !nomap {
			wsMergeLocals() // single-threaded merge of thread-local accumulators
		}
		switch {
		case nomap:
			checksumC = uint64(finalizeCNM())
		case noconcat, intcs:
			checksumC = uint64(finalizeCI())
		default:
			checksumC = finalizeC()
		}
		totalms = msSince(tTotal0)
	}
	done <- struct{}{}
}

func main() {
	prngSelfTest()

	workers = 1
	for _, a := range os.Args[1:] {
		if a == "--" {
			continue
		}
		if a == "intcs" {
			intcs = true
			continue
		}
		if a == "noconcat" {
			noconcat = true
			continue
		}
		if a == "nomap" {
			nomap = true
			continue
		}
		w, err := strconv.Atoi(a)
		if err != nil || w < 1 {
			fmt.Fprintf(os.Stderr, "usage: bench-go <W> [intcs|noconcat|nomap]\n")
			os.Exit(2)
		}
		workers = w
	}

	nBase = NBASEDEFAULT
	if os.Getenv("SCALEBENCH_SMOKE") == "1" {
		nBase = NBASESMOKE
	}
	nQueries = nBase

	wsMode = os.Getenv("SCALEBENCH_WS") == "1"
	if os.Getenv("SCALEBENCH_INTCS") == "1" {
		intcs = true
	}

	initShards()
	if nomap {
		initNM()
		initNMGroups()
	} else {
		initGroups()
	}
	if wsMode {
		initWS()
	}
	bar = newBarrier(workers)

	done := make(chan struct{}, workers) // join only — NOT a work queue
	for i := 0; i < workers; i++ {
		go worker(i, done)
	}
	for i := 0; i < workers; i++ {
		<-done
	}

	mode := ""
	if wsMode {
		mode = "\"mode\":\"ws\","
	}
	armTag := ""
	switch {
	case nomap:
		armTag = "\"arm\":\"nomap\","
	case noconcat:
		armTag = "\"arm\":\"noconcat\","
	case intcs:
		armTag = "\"intcs\":true,"
	}
	fmt.Printf("{\"impl\":\"go\","+armTag+"\"threads\":%d,"+mode+
		"\"phaseA_ms\":%.3f,\"phaseB_ms\":%.3f,\"phaseC_ms\":%.3f,\"total_ms\":%.3f,"+
		"\"checksumA\":\"%016x\",\"postings\":%d,\"checksumA2\":\"%016x\","+
		"\"checksumB\":\"%016x\",\"checksumC\":\"%016x\","+
		"\"docsIngested\":%d,\"tokensProcessed\":%d,\"writesDone\":%d}\n",
		workers,
		phaseAms, phaseBms, phaseCms, totalms,
		checksumA, postingsCount, checksumA2,
		checksumB, checksumC,
		docsIngested.Load(), tokensProcessed.Load(), writesDone.Load())
}
