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
	bar.await() // all workers spawned; clock starts
	if id == 0 {
		tTotal0 = time.Now()
		tPhase = tTotal0
	}

	phaseAWork()
	bar.await() // Phase A done
	if id == 0 {
		phaseAms = msSince(tPhase)
		checksumA, postingsCount = indexChecksum() // after the barrier, thread 0
		buildDfSnap()                              // frozen df snapshot, published before B
		tPhase = time.Now()
	}
	bar.await() // dfSnap published; Phase B starts

	phaseBWork()
	bar.await() // Phase B done
	if id == 0 {
		phaseBms = msSince(tPhase)
		checksumB = checksumBAcc.Load()
		checksumA2, _ = indexChecksum() // base + writer docs
		tPhase = time.Now()
	}
	bar.await() // Phase C starts

	phaseCWork()
	bar.await() // Phase C done
	if id == 0 {
		phaseCms = msSince(tPhase)
		checksumC = finalizeC()
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
		w, err := strconv.Atoi(a)
		if err != nil || w < 1 {
			fmt.Fprintf(os.Stderr, "usage: bench-go <W>\n")
			os.Exit(2)
		}
		workers = w
	}

	nBase = NBASEDEFAULT
	if os.Getenv("SCALEBENCH_SMOKE") == "1" {
		nBase = NBASESMOKE
	}
	nQueries = nBase

	initShards()
	initGroups()
	bar = newBarrier(workers)

	done := make(chan struct{}, workers) // join only — NOT a work queue
	for i := 0; i < workers; i++ {
		go worker(i, done)
	}
	for i := 0; i < workers; i++ {
		<-done
	}

	fmt.Printf("{\"impl\":\"go\",\"threads\":%d,"+
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
