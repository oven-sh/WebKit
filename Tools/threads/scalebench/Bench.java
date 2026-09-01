// Bench.java — SCALEBENCH Java implementation (Tools/threads/scalebench/SPEC.md, v1).
// Java 21, single file, no external deps. Run: java Bench <W>
//
// Fairness notes (SPEC §2):
//   - Plain java.lang.Thread (platform threads), one per worker.
//   - HashMap + ReentrantLock per shard. No ConcurrentHashMap / StampedLock /
//     LongAdder / ConcurrentSkipListMap anywhere.
//   - Hand-rolled counting barrier on ReentrantLock + Condition (NOT CyclicBarrier).
//   - All PRNG/hash/checksum arithmetic is u64-with-wraparound via Java long
//     (two's-complement wrap is bit-identical); unsigned semantics via
//     Long.remainderUnsigned / Long.divideUnsigned / Long.compareUnsigned.
//   - No floating point in the measured program (timing excepted).
//   - Posting list shape: two parallel growable arrays (docIds[], tfs[]) per SPEC §1.7.
//
// Interpretation notes (must match bench.js / bench.go — flagged for cross-check):
//   - Phase C "firstLetter": first letter of the base26 part, i.e. term.charAt(1)
//     (term.charAt(0) is always 't'; SPEC's "up to 104 groups" and the "3:a" key
//     example only make sense with the varying letter).
//   - Phase times: barrier-to-barrier around each parallel region, measured by
//     thread 0. total_ms = wall time from Phase A start barrier to after the
//     final checksumC computation (includes the thread-0 serial inter-phase
//     sections: dfSnap build, checksumA, checksumA2, group sort).
//   - queriesDone is incremented once per reader op (SPEC §1.7 lists the counter;
//     §1.9 does not name the increment site). Not part of the output JSON.
//   - Phase B writers run "exactly as in Phase A" including step 4, so
//     docsIngested/tokensProcessed include writer docs.
//   - SCALEBENCH_SMOKE=1 env => N_BASE=2000 (SPEC §5.1).
//   - CFG_N_BASE is the §8-calibrated value pinned in SPEC §1.1 (28000,
//     2026-06-10); run.sh preflight cross-checks it against all three sources.

import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.locks.Condition;
import java.util.concurrent.locks.ReentrantLock;

public final class Bench {

    // ---- §1.1 constants (one config block; runner cross-checks via JSON) ----
    static final long GLOBAL_SEED = 0x5CA1AB1E0BADF00DL;
    static final long QUERY_SEED  = 0xFACEFEEDC0FFEE11L;
    static final long GOLDEN      = 0x9E3779B97F4A7C15L;
    static final int  V           = 65536;
    static final int  K           = 128;
    static final long CFG_N_BASE  = 28000;    // pinned by §8 calibration 2026-06-10 (must match SPEC §1.1 / bench.js / main.go)
    static final int  WRITER_MOD  = 10;
    static final int  TOPN        = 20;

    static long N_BASE;
    static long N_QUERIES;
    static int  W;

    // ---- §1.2 PRNG: splitmix64 ----
    static final class Prng {
        long s;
        Prng(long seed) { s = seed; }
        long next() {
            s += GOLDEN;
            long z = s;
            z = (z ^ (z >>> 30)) * 0xBF58476D1CE4E5B9L;
            z = (z ^ (z >>> 27)) * 0x94D049BB133111EBL;
            return z ^ (z >>> 31);
        }
    }

    static long mix(long x) {
        long z = x + GOLDEN;
        z = (z ^ (z >>> 30)) * 0xBF58476D1CE4E5B9L;
        z = (z ^ (z >>> 27)) * 0x94D049BB133111EBL;
        return z ^ (z >>> 31);
    }

    static long randBelow(long r, long n) { return Long.remainderUnsigned(r, n); }

    // ---- §1.3 FNV-1a 64 ----
    static final long FNV_OFFSET = 0xCBF29CE484222325L;
    static final long FNV_PRIME  = 0x100000001B3L;

    static long fnv1a(String s) {
        long h = FNV_OFFSET;
        for (int i = 0; i < s.length(); i++)
            h = (h ^ (s.charAt(i) & 0xff)) * FNV_PRIME;
        return h;
    }

    static long fnv1aBytes(byte[] b) {
        long h = FNV_OFFSET;
        for (int i = 0; i < b.length; i++)
            h = (h ^ (b[i] & 0xff)) * FNV_PRIME;
        return h;
    }

    // ---- §1.4 vocabulary ----
    static String termString(int i) {
        char[] buf = new char[8];
        int p = 8;
        int x = i;
        do { buf[--p] = (char) ('a' + (x % 26)); x /= 26; } while (x != 0);
        buf[--p] = 't';
        return new String(buf, p, 8 - p);
    }

    static int pickTerm(Prng prng) {
        long a = randBelow(prng.next(), V);
        long b = randBelow(prng.next(), V);
        return (int) Math.min(a, b);
    }

    // ---- §1.5 document grammar ----
    static String genDocText(long d) {
        Prng prng = new Prng(mix(GLOBAL_SEED ^ (d * GOLDEN)));
        int titleLen = 5 + (int) randBelow(prng.next(), 8);
        int bodyLen  = 80 + (int) randBelow(prng.next(), 121);
        int n = titleLen + bodyLen;
        StringBuilder sb = new StringBuilder(n * 7);
        for (int j = 0; j < n; j++) {
            String tok = termString(pickTerm(prng));
            if (j > 0) sb.append(' ');
            if (j % 7 == 0) {
                sb.append((char) (tok.charAt(0) - 32));
                sb.append(tok, 1, tok.length());
            } else {
                sb.append(tok);
            }
            if (j % 11 == 3) sb.append(',');
            if (j % 13 == 12) sb.append('.');
        }
        return sb.toString();
    }

    // ---- §1.6 tokenizer (split on non-alnum, then allocate a lowercased copy of
    //      every piece — never skip the lowercase allocation) ----
    static ArrayList<String> tokenize(String text) {
        ArrayList<String> out = new ArrayList<>(256);
        int n = text.length();
        StringBuilder piece = new StringBuilder(8);
        for (int i = 0; i <= n; i++) {
            char c = i < n ? text.charAt(i) : ' ';
            boolean alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
            if (alnum) {
                piece.append(c);
            } else if (piece.length() > 0) {
                String raw = piece.toString();           // split allocation
                char[] lc = new char[raw.length()];      // lowercase allocation (always)
                for (int k = 0; k < raw.length(); k++) {
                    char ch = raw.charAt(k);
                    lc[k] = (ch >= 'A' && ch <= 'Z') ? (char) (ch + 32) : ch;
                }
                out.add(new String(lc));
                piece.setLength(0);
            }
        }
        return out;
    }

    // ---- §1.7 shared index ----
    static final class PostingList {
        long[] docIds = new long[4];
        int[]  tfs    = new int[4];
        int    size;
        void add(long d, int tf) {
            if (size == docIds.length) {
                docIds = Arrays.copyOf(docIds, size * 2);
                tfs    = Arrays.copyOf(tfs, size * 2);
            }
            docIds[size] = d;
            tfs[size] = tf;
            size++;
        }
    }

    static final class Shard {
        final ReentrantLock lock = new ReentrantLock();
        final HashMap<String, PostingList> map = new HashMap<>();
    }

    static Shard[] shards;

    static int shardOf(String term) { return (int) Long.remainderUnsigned(fnv1a(term), K); }

    static final AtomicLong docsIngested    = new AtomicLong();
    static final AtomicLong tokensProcessed = new AtomicLong();
    static final AtomicLong queriesDone     = new AtomicLong();
    static final AtomicLong writesDone      = new AtomicLong();

    static final AtomicLong nextDoc   = new AtomicLong();
    static final AtomicLong nextOp    = new AtomicLong();
    static final AtomicLong nextShard = new AtomicLong();

    static HashMap<String, Long> dfSnap; // built by thread 0 at the A/B barrier; read-only after

    // ---- §2.1 hand-rolled counting barrier (ReentrantLock + Condition) ----
    static final class Barrier {
        private final ReentrantLock lock = new ReentrantLock();
        private final Condition cond = lock.newCondition();
        private final int parties;
        private int waiting;
        private long gen;
        Barrier(int parties) { this.parties = parties; }
        void await() {
            lock.lock();
            try {
                long g = gen;
                waiting++;
                if (waiting == parties) {
                    waiting = 0;
                    gen++;
                    cond.signalAll();
                } else {
                    while (gen == g) cond.awaitUninterruptibly();
                }
            } finally {
                lock.unlock();
            }
        }
    }

    static Barrier barrier;

    // ---- ingest one doc (Phase A worker body; Phase B writers reuse verbatim) ----
    static void ingestDoc(long d) {
        String text = genDocText(d);
        ArrayList<String> toks = tokenize(text);
        HashMap<String, Integer> tf = new HashMap<>();
        for (String t : toks) tf.merge(t, 1, Integer::sum);
        for (var e : tf.entrySet()) {                 // one lock acquisition per (doc, distinct term)
            Shard sh = shards[shardOf(e.getKey())];
            sh.lock.lock();
            try {
                PostingList pl = sh.map.get(e.getKey());
                if (pl == null) { pl = new PostingList(); sh.map.put(e.getKey(), pl); }
                pl.add(d, e.getValue());
            } finally {
                sh.lock.unlock();
            }
        }
        tokensProcessed.addAndGet(toks.size());
        docsIngested.incrementAndGet();
    }

    // ---- §39b intcs arm (full-workload 32-bit, supersedes the §39 checksum-only
    //      reroute): SAME work shape as the spec arm — string concat → tokenize
    //      → HashMap<String>.get/set under shard locks → 90/10 query mix →
    //      Phase C group-by — but EVERY numeric operation is int (mod-2^32):
    //      splitmix32 PRNG (Prng32), docSeedI/qSeedI = mix32(seed^d), pickTermI,
    //      shardOfI = fnv1a32%K, mix32 everywhere, fnv1a32 for all string
    //      hashing, int per-thread partialB. Java int is signed two's-complement
    //      so *, +, ^ wrap mod-2^32 exactly like Go uint32 / JS Math.imul; >>>
    //      is logical shift; unsigned modulo via Integer.remainderUnsigned;
    //      unsigned compare via Integer.compareUnsigned. Produces DIFFERENT
    //      documents than the spec u64 arm and so a NEW cross-language reference
    //      tuple — recorded in docs/threads/SCALEBENCH.md §39b. Default OFF (no
    //      `intcs` arg / SCALEBENCH_INTCS unset ⇒ spec 64-bit path, b3e65a68…
    //      reference unchanged).
    static final int GSEED32  = 0x5ca1ab1e;
    static final int QSEED32  = 0xfacefeed;
    static final int GOLDEN32 = 0x9e3779b9;

    static int fnv1a32(String s) {
        int h = 0x811c9dc5;
        for (int i = 0; i < s.length(); i++)
            h = (h ^ s.charAt(i)) * 0x01000193;
        return h;
    }
    static int mix32(int x) {
        int z = x + 0x9e3779b9;
        z = (z ^ (z >>> 16)) * 0x85ebca6b;
        z = (z ^ (z >>> 13)) * 0xc2b2ae35;
        return z ^ (z >>> 16);
    }
    static final class Prng32 {
        int s;
        Prng32(int seed) { s = seed; }
        int next() {
            s += GOLDEN32;
            int z = s;
            z = (z ^ (z >>> 16)) * 0x85ebca6b;
            z = (z ^ (z >>> 13)) * 0xc2b2ae35;
            return z ^ (z >>> 16);
        }
    }
    static int randBelow32(int r, int n) { return Integer.remainderUnsigned(r, n); }
    static int docSeedI(long d) { return mix32(GSEED32 ^ (int) d); }
    static int qSeedI(long q)   { return mix32(QSEED32 ^ (int) q); }
    static int shardOfI(String term) { return Integer.remainderUnsigned(fnv1a32(term), K); }
    static int pickTermI(Prng32 p) {
        int a = p.next() & (V - 1);
        int b = p.next() & (V - 1);
        return Math.min(a, b);
    }
    static int fnvU32LE(int h, int x) {
        for (int i = 0; i < 4; i++) {
            h = (h ^ (x & 0xff)) * 0x01000193;
            x >>>= 8;
        }
        return h;
    }

    static String genDocTextI(long d) {
        Prng32 p = new Prng32(docSeedI(d));
        int titleLen = 5 + randBelow32(p.next(), 8);
        int bodyLen  = 80 + randBelow32(p.next(), 121);
        int n = titleLen + bodyLen;
        StringBuilder sb = new StringBuilder(n * 7);
        for (int j = 0; j < n; j++) {
            String tok = termString(pickTermI(p));
            if (j > 0) sb.append(' ');
            if (j % 7 == 0) {
                sb.append((char) (tok.charAt(0) - 32));
                sb.append(tok, 1, tok.length());
            } else {
                sb.append(tok);
            }
            if (j % 11 == 3) sb.append(',');
            if (j % 13 == 12) sb.append('.');
        }
        return sb.toString();
    }

    static void ingestDocI(long d) {
        String text = genDocTextI(d);
        ArrayList<String> toks = tokenize(text);
        HashMap<String, Integer> tf = new HashMap<>();
        for (String t : toks) tf.merge(t, 1, Integer::sum);
        for (var e : tf.entrySet()) {
            Shard sh = shards[shardOfI(e.getKey())];
            sh.lock.lock();
            try {
                PostingList pl = sh.map.get(e.getKey());
                if (pl == null) { pl = new PostingList(); sh.map.put(e.getKey(), pl); }
                pl.add(d, e.getValue());
            } finally {
                sh.lock.unlock();
            }
        }
        tokensProcessed.addAndGet(toks.size());
        docsIngested.incrementAndGet();
    }

    static long[] indexChecksum32() {
        int sum = 0;
        long count = 0;
        for (Shard sh : shards) {
            for (var e : sh.map.entrySet()) {
                int th = fnv1a32(e.getKey());
                PostingList pl = e.getValue();
                for (int i = 0; i < pl.size; i++) {
                    int item = th ^ (((int) pl.docIds[i]) * 0xd6e8feb9)
                                  ^ (pl.tfs[i] * 0xcaaf00dd);
                    sum += mix32(item);
                    count++;
                }
            }
        }
        return new long[] { ((long) sum) & 0xFFFFFFFFL, count };
    }

    static Snap snapshotI(String term) {
        Shard sh = shards[shardOfI(term)];
        Snap s = new Snap();
        sh.lock.lock();
        try {
            PostingList pl = sh.map.get(term);
            if (pl == null) {
                s.d = new long[0];
                s.tf = new int[0];
            } else {
                s.d = Arrays.copyOf(pl.docIds, pl.size);
                s.tf = Arrays.copyOf(pl.tfs, pl.size);
                s.n = pl.size;
            }
        } finally {
            sh.lock.unlock();
        }
        return s;
    }

    static int pointQueryI(Prng32 p) {
        String term = termString(pickTermI(p));
        int df = 0, sumTf = 0;
        Shard sh = shards[shardOfI(term)];
        sh.lock.lock();
        try {
            PostingList pl = sh.map.get(term);
            if (pl != null) {
                for (int i = 0; i < pl.size; i++) {
                    if (pl.docIds[i] < N_BASE) { df++; sumTf += pl.tfs[i]; }
                }
            }
        } finally {
            sh.lock.unlock();
        }
        return mix32(fnv1a32(term) ^ (df * 0x9e37) ^ sumTf);
    }

    static int andQueryI(Prng32 p) {
        int nTerms = 2 + randBelow32(p.next(), 2);
        String[] terms = new String[nTerms];
        for (int i = 0; i < nTerms; i++) terms[i] = termString(pickTermI(p));
        Snap[] snaps = new Snap[nTerms];
        for (int i = 0; i < nTerms; i++) snaps[i] = snapshotI(terms[i]);
        HashMap<Long, Integer> cand = new HashMap<>();
        for (int i = 0; i < snaps[0].n; i++) {
            if (snaps[0].d[i] < N_BASE) cand.put(snaps[0].d[i], 1);
        }
        for (int li = 1; li < nTerms; li++) {
            Snap s = snaps[li];
            for (int i = 0; i < s.n; i++) {
                if (s.d[i] >= N_BASE) continue;
                Integer c = cand.get(s.d[i]);
                if (c != null && c == li) cand.put(s.d[i], li + 1);
            }
        }
        int sum = 0, matchCount = 0;
        for (var e : cand.entrySet()) {
            if (e.getValue() == nTerms) { sum += mix32(e.getKey().intValue()); matchCount++; }
        }
        return mix32(sum) ^ matchCount;
    }

    static int scoredQueryI(Prng32 p) {
        int nTerms = 2 + randBelow32(p.next(), 2);
        String[] terms = new String[nTerms];
        for (int i = 0; i < nTerms; i++) terms[i] = termString(pickTermI(p));
        HashMap<Long, Integer> scores = new HashMap<>();
        for (int li = 0; li < nTerms; li++) {
            Long dfv = dfSnap.get(terms[li]);
            int idf = (dfv == null || dfv == 0) ? 0 : (int) (N_BASE * 1000L / dfv);
            Snap s = snapshotI(terms[li]);
            for (int i = 0; i < s.n; i++) {
                if (s.d[i] < N_BASE) scores.merge(s.d[i], s.tf[i] * idf, Integer::sum);
            }
        }
        ArrayList<long[]> list = new ArrayList<>(scores.size());
        for (var e : scores.entrySet())
            list.add(new long[] { e.getKey(), ((long) e.getValue()) & 0xFFFFFFFFL });
        list.sort((a, b) -> {
            int c = Long.compare(b[1], a[1]);             // score DESC (unsigned u32, stored as nonneg long)
            return c != 0 ? c : Long.compare(a[0], b[0]); // docId ASC
        });
        int n = Math.min(TOPN, list.size());
        int h = 0x811c9dc5;
        for (int i = 0; i < n; i++) h = fnvU32LE(h, (int) list.get(i)[0]);
        for (int i = 0; i < n; i++) h = fnvU32LE(h, (int) list.get(i)[1]);
        return h;
    }

    static int phaseBWorkI() {
        int local = 0;
        for (;;) {
            long q = nextOp.getAndIncrement();
            if (q >= N_QUERIES) break;
            if (q % WRITER_MOD == 0) {
                ingestDocI(N_BASE + q / WRITER_MOD);
                writesDone.incrementAndGet();
                continue;
            }
            Prng32 p = new Prng32(qSeedI(q));
            int kind = randBelow32(p.next(), 10);
            int h;
            if (kind < 4)      h = pointQueryI(p);
            else if (kind < 8) h = andQueryI(p);
            else               h = scoredQueryI(p);
            local += mix32((((int) q) * GOLDEN32) ^ h);
            queriesDone.incrementAndGet();
        }
        return local;
    }

    // ---- §40 NOCONCAT ARM — intcs base with the genDocTextI→tokenize
    //      round-trip removed: genDocTermsI emits the term strings directly.
    //      Everything downstream (tf HashMap<String>, shardOfI, shard.map
    //      get/put, queries, phaseC) is byte-identical to intcs.
    static ArrayList<String> genDocTermsI(long d) {
        Prng32 p = new Prng32(docSeedI(d));
        int titleLen = 5 + randBelow32(p.next(), 8);
        int bodyLen  = 80 + randBelow32(p.next(), 121);
        int n = titleLen + bodyLen;
        ArrayList<String> toks = new ArrayList<>(n);
        for (int j = 0; j < n; j++) toks.add(termString(pickTermI(p)));
        return toks;
    }

    static void ingestDocNC(long d) {
        ArrayList<String> toks = genDocTermsI(d);
        HashMap<String, Integer> tf = new HashMap<>();
        for (String t : toks) tf.merge(t, 1, Integer::sum);
        for (var e : tf.entrySet()) {
            Shard sh = shards[shardOfI(e.getKey())];
            sh.lock.lock();
            try {
                PostingList pl = sh.map.get(e.getKey());
                if (pl == null) { pl = new PostingList(); sh.map.put(e.getKey(), pl); }
                pl.add(d, e.getValue());
            } finally {
                sh.lock.unlock();
            }
        }
        tokensProcessed.addAndGet(toks.size());
        docsIngested.incrementAndGet();
    }

    static int phaseBWorkNC() {
        int local = 0;
        for (;;) {
            long q = nextOp.getAndIncrement();
            if (q >= N_QUERIES) break;
            if (q % WRITER_MOD == 0) {
                ingestDocNC(N_BASE + q / WRITER_MOD);
                writesDone.incrementAndGet();
                continue;
            }
            Prng32 p = new Prng32(qSeedI(q));
            int kind = randBelow32(p.next(), 10);
            int h;
            if (kind < 4)      h = pointQueryI(p);
            else if (kind < 8) h = andQueryI(p);
            else               h = scoredQueryI(p);
            local += mix32((((int) q) * GOLDEN32) ^ h);
            queriesDone.incrementAndGet();
        }
        return local;
    }

    // ---- §40 NOMAP ARM — intcs base with every Map<String> lookup replaced
    //      by a direct integer index. Full string round-trip (genDocTextI →
    //      tokenize) KEPT.
    static int invTermOf(String s) {
        int r = 0;
        for (int i = 1; i < s.length(); i++)
            r = r * 26 + (s.charAt(i) - 'a');
        return r;
    }
    static int firstLetterIdx(int t) {
        while (t >= 26) t /= 26;
        return t;
    }
    static int termLenBucket(int t) {
        return t < 26 ? 2 : t < 676 ? 3 : t < 17576 ? 4 : 5;
    }

    static int[] nmShardOf;       // nmShardOf[t] = fnv1a32(termString(t)) % K
    static PostingList[] nmPost;  // V slots; null == empty
    static int[] nmDfSnap;

    static final class NmScratch {
        final int[] tf = new int[V];
        final int[] dirty = new int[256];
    }

    static void initNM() {
        nmShardOf = new int[V];
        for (int t = 0; t < V; t++)
            nmShardOf[t] = Integer.remainderUnsigned(fnv1a32(termString(t)), K);
        nmPost = new PostingList[V];
    }

    static void ingestDocNM(long d, NmScratch sc) {
        String text = genDocTextI(d);
        ArrayList<String> toks = tokenize(text);
        int nDirty = 0;
        for (String w : toks) {
            int t = invTermOf(w);
            if (sc.tf[t] == 0) sc.dirty[nDirty++] = t;
            sc.tf[t]++;
        }
        for (int k = 0; k < nDirty; k++) {
            int t = sc.dirty[k];
            int c = sc.tf[t];
            sc.tf[t] = 0;
            Shard sh = shards[nmShardOf[t]];
            sh.lock.lock();
            try {
                PostingList pl = nmPost[t];
                if (pl == null) { pl = new PostingList(); nmPost[t] = pl; }
                pl.add(d, c);
            } finally {
                sh.lock.unlock();
            }
        }
        tokensProcessed.addAndGet(toks.size());
        docsIngested.incrementAndGet();
    }

    static long[] indexChecksumNM() {
        int sum = 0;
        long count = 0;
        for (int t = 0; t < V; t++) {
            PostingList pl = nmPost[t];
            if (pl == null) continue;
            int th = mix32(t);
            for (int i = 0; i < pl.size; i++) {
                int item = th ^ (((int) pl.docIds[i]) * 0xd6e8feb9)
                              ^ (pl.tfs[i] * 0xcaaf00dd);
                sum += mix32(item);
                count++;
            }
        }
        return new long[] { ((long) sum) & 0xFFFFFFFFL, count };
    }

    static void buildDfSnapNM() {
        nmDfSnap = new int[V];
        for (int t = 0; t < V; t++) {
            PostingList pl = nmPost[t];
            if (pl != null) nmDfSnap[t] = pl.size;
        }
    }

    static Snap snapshotNM(int t) {
        Shard sh = shards[nmShardOf[t]];
        Snap s = new Snap();
        sh.lock.lock();
        try {
            PostingList pl = nmPost[t];
            if (pl == null) {
                s.d = new long[0]; s.tf = new int[0];
            } else {
                s.d = Arrays.copyOf(pl.docIds, pl.size);
                s.tf = Arrays.copyOf(pl.tfs, pl.size);
                s.n = pl.size;
            }
        } finally {
            sh.lock.unlock();
        }
        return s;
    }

    static int pointQueryNM(Prng32 p) {
        int t = pickTermI(p);
        int df = 0, sumTf = 0;
        Shard sh = shards[nmShardOf[t]];
        sh.lock.lock();
        try {
            PostingList pl = nmPost[t];
            if (pl != null) {
                for (int i = 0; i < pl.size; i++) {
                    if (pl.docIds[i] < N_BASE) { df++; sumTf += pl.tfs[i]; }
                }
            }
        } finally {
            sh.lock.unlock();
        }
        return mix32(mix32(t) ^ (df * 0x9e37) ^ sumTf);
    }

    static int andQueryNM(Prng32 p) {
        int nTerms = 2 + randBelow32(p.next(), 2);
        Snap[] snaps = new Snap[nTerms];
        for (int i = 0; i < nTerms; i++) snaps[i] = snapshotNM(pickTermI(p));
        HashMap<Long, Integer> cand = new HashMap<>();
        for (int i = 0; i < snaps[0].n; i++) {
            if (snaps[0].d[i] < N_BASE) cand.put(snaps[0].d[i], 1);
        }
        for (int li = 1; li < nTerms; li++) {
            Snap s = snaps[li];
            for (int i = 0; i < s.n; i++) {
                if (s.d[i] >= N_BASE) continue;
                Integer c = cand.get(s.d[i]);
                if (c != null && c == li) cand.put(s.d[i], li + 1);
            }
        }
        int sum = 0, matchCount = 0;
        for (var e : cand.entrySet()) {
            if (e.getValue() == nTerms) { sum += mix32(e.getKey().intValue()); matchCount++; }
        }
        return mix32(sum) ^ matchCount;
    }

    static int scoredQueryNM(Prng32 p) {
        int nTerms = 2 + randBelow32(p.next(), 2);
        HashMap<Long, Integer> scores = new HashMap<>();
        for (int li = 0; li < nTerms; li++) {
            int t = pickTermI(p);
            int df = nmDfSnap[t];
            int idf = df == 0 ? 0 : (int) (N_BASE * 1000L / df);
            Snap s = snapshotNM(t);
            for (int i = 0; i < s.n; i++) {
                if (s.d[i] < N_BASE) scores.merge(s.d[i], s.tf[i] * idf, Integer::sum);
            }
        }
        ArrayList<long[]> list = new ArrayList<>(scores.size());
        for (var e : scores.entrySet())
            list.add(new long[] { e.getKey(), ((long) e.getValue()) & 0xFFFFFFFFL });
        list.sort((a, b) -> {
            int c = Long.compare(b[1], a[1]);
            return c != 0 ? c : Long.compare(a[0], b[0]);
        });
        int n = Math.min(TOPN, list.size());
        int h = 0x811c9dc5;
        for (int i = 0; i < n; i++) h = fnvU32LE(h, (int) list.get(i)[0]);
        for (int i = 0; i < n; i++) h = fnvU32LE(h, (int) list.get(i)[1]);
        return h;
    }

    static int phaseBWorkNM(NmScratch sc) {
        int local = 0;
        for (;;) {
            long q = nextOp.getAndIncrement();
            if (q >= N_QUERIES) break;
            if (q % WRITER_MOD == 0) {
                ingestDocNM(N_BASE + q / WRITER_MOD, sc);
                writesDone.incrementAndGet();
                continue;
            }
            Prng32 p = new Prng32(qSeedI(q));
            int kind = randBelow32(p.next(), 10);
            int h;
            if (kind < 4)      h = pointQueryNM(p);
            else if (kind < 8) h = andQueryNM(p);
            else               h = scoredQueryNM(p);
            local += mix32((((int) q) * GOLDEN32) ^ h);
            queriesDone.incrementAndGet();
        }
        return local;
    }

    static final class NmGroup {
        final ReentrantLock lock = new ReentrantLock();
        long totalTf, df;
        final ArrayList<int[]> terms = new ArrayList<>(); // {termId, totalTf}
    }
    static NmGroup[] nmGroups;

    static void phaseCWorkNM() {
        int block = V / K; // 512
        for (;;) {
            long s = nextShard.getAndIncrement();
            if (s >= K) return;
            int lo = (int) s * block, hi = lo + block;
            for (int t = lo; t < hi; t++) {
                PostingList pl = nmPost[t];
                if (pl == null) continue;
                long tot = 0;
                for (int i = 0; i < pl.size; i++) tot += pl.tfs[i];
                int gk = (termLenBucket(t) - 2) * 26 + firstLetterIdx(t);
                NmGroup g = nmGroups[gk];
                g.lock.lock();
                try {
                    g.totalTf += tot;
                    g.df += pl.size;
                    g.terms.add(new int[] { t, (int) tot });
                } finally {
                    g.lock.unlock();
                }
            }
        }
    }

    static int finalizeCNM() {
        int sum = 0;
        for (int gk = 0; gk < 104; gk++) {
            NmGroup g = nmGroups[gk];
            g.terms.sort((a, b) -> {
                if (a[1] != b[1]) return Integer.compare(b[1], a[1]);
                return Integer.compare(a[0], b[0]);
            });
            int n = Math.min(TOPN, g.terms.size());
            StringBuilder sb = new StringBuilder();
            for (int i = 0; i < n; i++) {
                int[] e = g.terms.get(i);
                sb.append(termString(e[0])).append(':').append(e[1]).append(',');
            }
            int len = gk / 26 + 2;
            String key = len + ":" + (char) ('a' + gk % 26);
            sum += mix32(fnv1a32(key) ^ ((int) g.totalTf)
                ^ (((int) g.df) * GOLDEN32) ^ fnv1a32(sb.toString()));
        }
        return sum;
    }

    static int finalizeCI() {
        int sum = 0;
        for (var ge : groups.entrySet()) {
            Group g = ge.getValue();
            g.terms.sort((a, b) -> {
                int c = Long.compareUnsigned(b.tt, a.tt);
                return c != 0 ? c : a.term.compareTo(b.term);
            });
            int n = Math.min(TOPN, g.terms.size());
            StringBuilder sb = new StringBuilder();
            for (int i = 0; i < n; i++) {
                sb.append(g.terms.get(i).term).append(':')
                  .append(Long.toUnsignedString(g.terms.get(i).tt)).append(',');
            }
            sum += mix32(fnv1a32(ge.getKey()) ^ ((int) g.totalTf)
                ^ (((int) g.df) * GOLDEN32) ^ fnv1a32(sb.toString()));
        }
        return sum;
    }

    // ---- §1.8 index checksum: returns {checksum, postingsCount} ----
    static long[] indexChecksum() {
        long sum = 0, count = 0;
        for (Shard sh : shards) {
            for (var e : sh.map.entrySet()) {
                long th = fnv1a(e.getKey());
                PostingList pl = e.getValue();
                for (int i = 0; i < pl.size; i++) {
                    sum += mix(th ^ (pl.docIds[i] * 0xD6E8FEB86659FD93L)
                                  ^ (((long) pl.tfs[i]) * 0xCAAF00DDL));
                    count++;
                }
            }
        }
        return new long[] { sum, count };
    }

    // ---- §1.9 readers ----
    static long pointQuery(Prng prng) {
        String term = termString(pickTerm(prng));
        long df = 0, sumTf = 0;
        Shard sh = shards[shardOf(term)];
        sh.lock.lock();
        try {
            PostingList pl = sh.map.get(term);
            if (pl != null) {
                for (int i = 0; i < pl.size; i++) {
                    if (pl.docIds[i] < N_BASE) { df++; sumTf += pl.tfs[i]; }
                }
            }
        } finally {
            sh.lock.unlock();
        }
        return mix(fnv1a(term) ^ (df * 0x9E37L) ^ sumTf);
    }

    static final class Snap {
        long[] d;
        int[]  tf;
        int    n;
    }

    static Snap snapshot(String term) {
        Shard sh = shards[shardOf(term)];
        Snap s = new Snap();
        sh.lock.lock();
        try {
            PostingList pl = sh.map.get(term);
            if (pl == null) {
                s.d = new long[0];
                s.tf = new int[0];
            } else {
                s.d = Arrays.copyOf(pl.docIds, pl.size);
                s.tf = Arrays.copyOf(pl.tfs, pl.size);
                s.n = pl.size;
            }
        } finally {
            sh.lock.unlock();
        }
        return s;
    }

    static long andQuery(Prng prng) {
        int nTerms = 2 + (int) randBelow(prng.next(), 2);
        String[] terms = new String[nTerms];
        for (int i = 0; i < nTerms; i++) terms[i] = termString(pickTerm(prng));
        Snap[] snaps = new Snap[nTerms];
        for (int i = 0; i < nTerms; i++) snaps[i] = snapshot(terms[i]);
        HashMap<Long, Integer> cand = new HashMap<>();
        for (int i = 0; i < snaps[0].n; i++) {
            if (snaps[0].d[i] < N_BASE) cand.put(snaps[0].d[i], 1);
        }
        for (int li = 1; li < nTerms; li++) {
            Snap s = snaps[li];
            for (int i = 0; i < s.n; i++) {
                if (s.d[i] >= N_BASE) continue;
                Integer c = cand.get(s.d[i]);
                if (c != null && c == li) cand.put(s.d[i], li + 1);
            }
        }
        long sum = 0, matchCount = 0;
        for (var e : cand.entrySet()) {
            if (e.getValue() == nTerms) { sum += mix(e.getKey()); matchCount++; }
        }
        return mix(sum) ^ matchCount;
    }

    static long scoredQuery(Prng prng) {
        int nTerms = 2 + (int) randBelow(prng.next(), 2);
        String[] terms = new String[nTerms];
        for (int i = 0; i < nTerms; i++) terms[i] = termString(pickTerm(prng));
        HashMap<Long, Long> scores = new HashMap<>();
        for (int li = 0; li < nTerms; li++) {
            Long dfv = dfSnap.get(terms[li]);
            long idf = (dfv == null) ? 0 : Long.divideUnsigned(N_BASE * 1000L, dfv);
            Snap s = snapshot(terms[li]);
            for (int i = 0; i < s.n; i++) {
                if (s.d[i] < N_BASE) scores.merge(s.d[i], ((long) s.tf[i]) * idf, Long::sum);
            }
        }
        ArrayList<long[]> list = new ArrayList<>(scores.size());
        for (var e : scores.entrySet()) list.add(new long[] { e.getKey(), e.getValue() });
        list.sort((a, b) -> {
            int c = Long.compareUnsigned(b[1], a[1]);    // score DESC
            return c != 0 ? c : Long.compare(a[0], b[0]); // docId ASC
        });
        int n = Math.min(TOPN, list.size());
        byte[] bytes = new byte[16 * n];
        for (int i = 0; i < n; i++) putLE(bytes, 8 * i, list.get(i)[0]);
        for (int i = 0; i < n; i++) putLE(bytes, 8 * n + 8 * i, list.get(i)[1]);
        return fnv1aBytes(bytes);
    }

    static void putLE(byte[] b, int off, long v) {
        for (int i = 0; i < 8; i++) b[off + i] = (byte) (v >>> (8 * i));
    }

    // ---- §1.10 Phase C groups ----
    static final class TermTf {
        final String term;
        final long   tt;
        TermTf(String term, long tt) { this.term = term; this.tt = tt; }
    }

    static final class Group {
        final ReentrantLock lock = new ReentrantLock();
        long totalTf, df;
        final ArrayList<TermTf> terms = new ArrayList<>();
    }

    static HashMap<String, Group> groups; // pre-populated with all 104 keys; outer map read-only

    static String groupKey(String term) {
        // firstLetter = first letter of the base26 part (term.charAt(0) is always 't')
        return term.length() + ":" + term.charAt(1);
    }

    // ---- §1.10-WS Phase C work-stealing arm (mode-selected, env SCALEBENCH_WS=1;
    //      the naive inline Phase C loop in worker() is untouched).
    //
    // Identical algorithm to bench.js phaseCWS / go phaseCWSWork:
    //   - W per-worker deques over the K shard indices; shard s seeded into
    //     deque s % W in increasing s order. Fixed array, [head, tail) live
    //     region, plain ReentrantLock per deque (the one mutex type, §2.1 —
    //     no lock-free Chase-Lev in any language).
    //   - Owner pops from the TAIL; when empty, scan victims (w+1..w+W-1
    //     mod W) and steal ceil(n/2) from the victim's HEAD (victim lock for
    //     the copy, then own lock to append — never nested).
    //   - Shared atomic wsRemaining (init K) decremented per popped shard; a
    //     worker exits when own deque empty + full scan failed + remaining==0.
    //   - THREAD-LOCAL accumulation (HashMap groupKey -> LocalGroup, no group
    //     locks); thread 0 merges the W local maps single-threaded after the
    //     Phase C barrier, then the naive sort/topN/checksumC code runs
    //     unchanged.
    static boolean WS_MODE;
    static boolean INTCS;
    static boolean NOCONCAT;
    static boolean NOMAP;

    static final class WsDeque {
        final ReentrantLock lock = new ReentrantLock();
        int[] arr = new int[K];
        int head, tail;
    }

    static final class LocalGroup {
        long totalTf, df;
        final ArrayList<TermTf> terms = new ArrayList<>();
    }

    static WsDeque[] wsDeques;
    static HashMap<String, LocalGroup>[] wsLocals;
    static final AtomicLong wsRemaining = new AtomicLong();

    @SuppressWarnings("unchecked")
    static void initWS() {
        wsDeques = new WsDeque[W];
        for (int w = 0; w < W; w++) wsDeques[w] = new WsDeque();
        for (int s = 0; s < K; s++) {
            WsDeque dq = wsDeques[s % W];
            if (dq.tail == dq.arr.length) dq.arr = Arrays.copyOf(dq.arr, dq.arr.length * 2);
            dq.arr[dq.tail++] = s;
        }
        wsLocals = new HashMap[W];
        wsRemaining.set(K);
    }

    static int wsPopOwn(int w) {
        WsDeque dq = wsDeques[w];
        dq.lock.lock();
        try {
            if (dq.head < dq.tail) return dq.arr[--dq.tail];
            return -1;
        } finally {
            dq.lock.unlock();
        }
    }

    static boolean wsSteal(int w) {
        for (int off = 1; off < W; off++) {
            WsDeque dq = wsDeques[(w + off) % W];
            int[] stolen = null;
            dq.lock.lock();
            try {
                int n = dq.tail - dq.head;
                if (n > 0) {
                    int cnt = (n + 1) / 2; // steal-half = ceil(n/2), from the head
                    stolen = Arrays.copyOfRange(dq.arr, dq.head, dq.head + cnt);
                    dq.head += cnt;
                }
            } finally {
                dq.lock.unlock();
            }
            if (stolen != null) {
                WsDeque own = wsDeques[w];
                own.lock.lock();
                try {
                    while (own.tail + stolen.length > own.arr.length)
                        own.arr = Arrays.copyOf(own.arr, own.arr.length * 2);
                    System.arraycopy(stolen, 0, own.arr, own.tail, stolen.length);
                    own.tail += stolen.length;
                } finally {
                    own.lock.unlock();
                }
                return true;
            }
        }
        return false;
    }

    static void phaseCWS(int tid) {
        HashMap<String, LocalGroup> local = new HashMap<>(); // thread-local accumulator
        for (;;) {
            int si = wsPopOwn(tid);
            if (si < 0) {
                if (wsSteal(tid)) continue;
                if (wsRemaining.get() <= 0) break;
                continue; // transiently in-flight steal; re-scan
            }
            wsRemaining.decrementAndGet();
            Shard sh = shards[si];
            for (var e : sh.map.entrySet()) {
                String term = e.getKey();
                PostingList pl = e.getValue();
                long tt = 0;
                for (int i = 0; i < pl.size; i++) tt += pl.tfs[i];
                long df = pl.size; // ALL postings: base + writer docs
                LocalGroup g = local.computeIfAbsent(groupKey(term), k -> new LocalGroup());
                g.totalTf += tt;
                g.df += df;
                g.terms.add(new TermTf(term, tt));
            }
        }
        wsLocals[tid] = local;
    }

    // Thread 0, single-threaded after the Phase C barrier: merge the W
    // thread-local accumulators into the shared groups; the naive
    // sort/topN/checksumC code then runs unchanged.
    static void wsMergeLocals() {
        for (int w = 0; w < W; w++) {
            for (var e : wsLocals[w].entrySet()) {
                Group g = groups.get(e.getKey());
                LocalGroup lg = e.getValue();
                g.totalTf += lg.totalTf;
                g.df += lg.df;
                g.terms.addAll(lg.terms);
            }
        }
    }

    // ---- results (written by thread 0 between barriers; read by main after join) ----
    static long tStartA, tEndA, tStartB, tEndB, tStartC, tEndC, tEnd;
    static long checksumA, postingsCount, checksumA2, checksumC;
    static final AtomicLong checksumB = new AtomicLong();

    // ---- worker ----
    static void worker(int tid) {
        NmScratch nmSc = NOMAP ? new NmScratch() : null;
        barrier.await();                                  // start line: all threads spawned
        if (tid == 0) tStartA = System.nanoTime();

        // Phase A — INGEST
        for (;;) {
            long d = nextDoc.getAndIncrement();
            if (d >= N_BASE) break;
            if (NOMAP) ingestDocNM(d, nmSc);
            else if (NOCONCAT) ingestDocNC(d);
            else if (INTCS) ingestDocI(d);
            else ingestDoc(d);
        }
        barrier.await();
        if (tid == 0) {
            tEndA = System.nanoTime();
            if (NOMAP) {
                buildDfSnapNM();
            } else {
                HashMap<String, Long> snap = new HashMap<>();
                for (Shard sh : shards) {
                    for (var e : sh.map.entrySet()) snap.put(e.getKey(), (long) e.getValue().size);
                }
                dfSnap = snap;                            // published by the barrier below
            }
            long[] r = NOMAP ? indexChecksumNM()
                     : (INTCS || NOCONCAT) ? indexChecksum32() : indexChecksum();
            checksumA = r[0];
            postingsCount = r[1];
        }
        barrier.await();
        if (tid == 0) tStartB = System.nanoTime();

        // Phase B — QUERY (90% read / 10% write)
        if (NOMAP) {
            int localB = phaseBWorkNM(nmSc);
            checksumB.addAndGet(((long) localB) & 0xFFFFFFFFL);
        } else if (NOCONCAT) {
            int localB = phaseBWorkNC();
            checksumB.addAndGet(((long) localB) & 0xFFFFFFFFL);
        } else if (INTCS) {
            int localB = phaseBWorkI();
            checksumB.addAndGet(((long) localB) & 0xFFFFFFFFL);
        } else {
            long localB = 0;
            for (;;) {
                long q = nextOp.getAndIncrement();
                if (q >= N_QUERIES) break;
                Prng prng = new Prng(mix(QUERY_SEED ^ (q * GOLDEN)));
                if (q % WRITER_MOD == 0) {
                    ingestDoc(N_BASE + q / WRITER_MOD);
                    writesDone.incrementAndGet();
                } else {
                    long kind = randBelow(prng.next(), 10);
                    long h;
                    if (kind < 4)      h = pointQuery(prng);
                    else if (kind < 8) h = andQuery(prng);
                    else               h = scoredQuery(prng);
                    localB += mix((q * GOLDEN) ^ h);
                    queriesDone.incrementAndGet();
                }
            }
            checksumB.addAndGet(localB);
        }
        barrier.await();
        if (tid == 0) {
            tEndB = System.nanoTime();
            checksumA2 = (NOMAP ? indexChecksumNM()
                        : (INTCS || NOCONCAT) ? indexChecksum32() : indexChecksum())[0];
        }
        barrier.await();
        if (tid == 0) tStartC = System.nanoTime();

        // Phase C — ANALYTICS
        if (NOMAP)
            phaseCWorkNM();
        else if (WS_MODE)
            phaseCWS(tid);
        else
        for (;;) {
            long si = nextShard.getAndIncrement();
            if (si >= K) break;
            Shard sh = shards[(int) si];
            for (var e : sh.map.entrySet()) {
                String term = e.getKey();
                PostingList pl = e.getValue();
                long tt = 0;
                for (int i = 0; i < pl.size; i++) tt += pl.tfs[i];
                long df = pl.size;
                Group g = groups.get(groupKey(term));
                g.lock.lock();
                try {
                    g.totalTf += tt;
                    g.df += df;
                    g.terms.add(new TermTf(term, tt));
                } finally {
                    g.lock.unlock();
                }
            }
        }
        barrier.await();
        if (tid == 0) {
            tEndC = System.nanoTime();
            if (WS_MODE && !NOMAP) wsMergeLocals(); // single-threaded merge of thread-local accumulators
            if (NOMAP) {
                checksumC = ((long) finalizeCNM()) & 0xFFFFFFFFL;
                tEnd = System.nanoTime();
                return;
            }
            if (INTCS || NOCONCAT) {
                checksumC = ((long) finalizeCI()) & 0xFFFFFFFFL;
                tEnd = System.nanoTime();
                return;
            }
            long sum = 0;
            for (var ge : groups.entrySet()) {
                Group g = ge.getValue();
                g.terms.sort((a, b) -> {
                    int c = Long.compareUnsigned(b.tt, a.tt);   // totalTf DESC
                    return c != 0 ? c : a.term.compareTo(b.term); // term ASC
                });
                int n = Math.min(TOPN, g.terms.size());
                StringBuilder sb = new StringBuilder();
                for (int i = 0; i < n; i++) {
                    sb.append(g.terms.get(i).term).append(':')
                      .append(Long.toUnsignedString(g.terms.get(i).tt)).append(',');
                }
                sum += mix(fnv1a(ge.getKey()) ^ g.totalTf ^ (g.df * GOLDEN) ^ fnv1a(sb.toString()));
            }
            checksumC = sum;
            tEnd = System.nanoTime();
        }
    }

    // ---- §7 PRNG self-test (test vector from the spec) ----
    static void selfTestPrng() {
        Prng p = new Prng(0);
        if (p.next() != 0xE220A8397B1DCDAFL
                || p.next() != 0x6E789E6AA1B965F4L
                || p.next() != 0x06C45D188009454FL) {
            System.err.println("FATAL: splitmix64 self-test failed");
            System.exit(3);
        }
    }

    static String hex16(long v) { return String.format("%016x", v); }

    // Fractional milliseconds, 3 decimals (timing is explicitly exempt from the
    // SPEC §2.5 no-float rule). Integer-ms truncation would floor-bias high-W
    // Phase B/C cells (single-digit ms at W>=32) by up to 1 ms — a 5-20%
    // speedup inflation applied only to Go/Java; all three implementations
    // report sub-ms resolution instead (JS rounds to 3 decimals too).
    // Locale.ROOT pins the '.' decimal separator regardless of host locale.
    static String ms(long startNs, long endNs) {
        return String.format(java.util.Locale.ROOT, "%.3f", (endNs - startNs) / 1e6);
    }

    public static void main(String[] args) throws Exception {
        if (args.length < 1) {
            System.err.println("usage: java Bench <W>");
            System.exit(2);
        }
        W = Integer.parseInt(args[0]);
        if (W < 1) { System.err.println("W must be >= 1"); System.exit(2); }
        boolean smoke = "1".equals(System.getenv("SCALEBENCH_SMOKE"));
        N_BASE = smoke ? 2000 : CFG_N_BASE;
        N_QUERIES = N_BASE;
        WS_MODE = "1".equals(System.getenv("SCALEBENCH_WS"));
        INTCS = "1".equals(System.getenv("SCALEBENCH_INTCS"));
        for (int i = 1; i < args.length; i++) {
            if ("intcs".equals(args[i])) INTCS = true;
            if ("noconcat".equals(args[i])) NOCONCAT = true;
            if ("nomap".equals(args[i])) NOMAP = true;
        }

        selfTestPrng();

        shards = new Shard[K];
        for (int i = 0; i < K; i++) shards[i] = new Shard();

        if (NOMAP) {
            initNM();
            nmGroups = new NmGroup[104];
            for (int i = 0; i < 104; i++) nmGroups[i] = new NmGroup();
        } else {
            groups = new HashMap<>();
            for (int len = 2; len <= 5; len++) {
                for (char c = 'a'; c <= 'z'; c++) groups.put(len + ":" + c, new Group());
            }
        }

        if (WS_MODE) initWS();

        barrier = new Barrier(W);
        Thread[] ts = new Thread[W];
        for (int i = 0; i < W; i++) {
            final int tid = i;
            ts[i] = new Thread(() -> worker(tid), "scalebench-worker-" + tid);
            ts[i].start();
        }
        for (Thread t : ts) t.join();

        StringBuilder out = new StringBuilder(512);
        out.append("{\"impl\":\"java\"");
        if (NOMAP) out.append(",\"arm\":\"nomap\"");
        else if (NOCONCAT) out.append(",\"arm\":\"noconcat\"");
        else if (INTCS) out.append(",\"intcs\":true");
        out.append(",\"threads\":").append(W);
        if (WS_MODE) out.append(",\"mode\":\"ws\"");
        out.append(",\"phaseA_ms\":").append(ms(tStartA, tEndA))
           .append(",\"phaseB_ms\":").append(ms(tStartB, tEndB))
           .append(",\"phaseC_ms\":").append(ms(tStartC, tEndC))
           .append(",\"total_ms\":").append(ms(tStartA, tEnd))
           .append(",\"checksumA\":\"").append(hex16(checksumA)).append('"')
           .append(",\"postings\":").append(postingsCount)
           .append(",\"checksumA2\":\"").append(hex16(checksumA2)).append('"')
           .append(",\"checksumB\":\"").append(hex16((INTCS || NOCONCAT || NOMAP) ? (checksumB.get() & 0xFFFFFFFFL) : checksumB.get())).append('"')
           .append(",\"checksumC\":\"").append(hex16(checksumC)).append('"')
           .append(",\"docsIngested\":").append(docsIngested.get())
           .append(",\"tokensProcessed\":").append(tokensProcessed.get())
           .append(",\"writesDone\":").append(writesDone.get())
           .append('}');
        System.out.println(out);
    }
}
