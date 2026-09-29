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

// This is made by lib/convert-list-sort.py from Objects/listobject.c of CPython 3.14. It is not to be changed by hand. It is for PythonListSort.cpp and nothing else to include.
// Objects/listsort.txt of CPython says how it works, and why.

#pragma once

#include <wtf/FastMalloc.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace Python { namespace ListSortKernel {

// What is sorted. All that is done with one here is to move it about, and to hand it to what compares it with another.
using Item = EncodedJSValue;

static inline void* tryAllocate(size_t size)
{
    void* result;
    return tryFastMalloc(size).getValue(result) ? result : nullptr;
}

static inline void release(void* memory) { fastFree(memory); }

/* Reverse a slice of a list in place, from lo up to (exclusive) hi. */
static void
reverse_slice(Item* lo, Item* hi)
{
    ASSERT(lo && hi);

    --hi;
    while (lo < hi) {
        Item t = *lo;
        *lo = *hi;
        *hi = t;
        ++lo;
        --hi;
    }
}

/* Lots of code for an adaptive, stable, natural mergesort.  There are many
 * pieces to this algorithm; read listsort.txt for overviews and details.
 */

/* A sortslice contains a pointer to an array of keys and a pointer to
 * an array of corresponding values.  In other words, keys[i]
 * corresponds with values[i].  If values == nullptr, then the keys are
 * also the values.
 *
 * Several convenience routines are provided here, so that keys and
 * values are always moved in sync.
 */

struct sortslice {
    Item* keys;
    Item* values;
};

static inline void
sortslice_copy(sortslice *s1, ptrdiff_t i, sortslice *s2, ptrdiff_t j)
{
    s1->keys[i] = s2->keys[j];
    if (s1->values != nullptr)
        s1->values[i] = s2->values[j];
}

static inline void
sortslice_copy_incr(sortslice *dst, sortslice *src)
{
    *dst->keys++ = *src->keys++;
    if (dst->values != nullptr)
        *dst->values++ = *src->values++;
}

static inline void
sortslice_copy_decr(sortslice *dst, sortslice *src)
{
    *dst->keys-- = *src->keys--;
    if (dst->values != nullptr)
        *dst->values-- = *src->values--;
}


static inline void
sortslice_memcpy(sortslice *s1, ptrdiff_t i, sortslice *s2, ptrdiff_t j,
                 ptrdiff_t n)
{
    memcpy(&s1->keys[i], &s2->keys[j], sizeof(Item) * n);
    if (s1->values != nullptr)
        memcpy(&s1->values[i], &s2->values[j], sizeof(Item) * n);
}

static inline void
sortslice_memmove(sortslice *s1, ptrdiff_t i, sortslice *s2, ptrdiff_t j,
                  ptrdiff_t n)
{
    memmove(&s1->keys[i], &s2->keys[j], sizeof(Item) * n);
    if (s1->values != nullptr)
        memmove(&s1->values[i], &s2->values[j], sizeof(Item) * n);
}

static inline void
sortslice_advance(sortslice *slice, ptrdiff_t n)
{
    slice->keys += n;
    if (slice->values != nullptr)
        slice->values += n;
}

/* Comparison function: ms->key_compare, which is set at run-time in
 * listsort_impl to optimize for various special cases.
 * Returns -1 on error, 1 if x < y, 0 if x >= y.
 */

#define ISLT(X, Y) (*(ms->key_compare))(X, Y, ms)

/* Compare X to Y via "<".  Goto "fail" if the comparison raises an
   error.  Else "k" is set to true iff X<Y, and an "if (k)" block is
   started.  It makes more sense in context <wink>.  X and Y are PyObject*s.
*/
#define IFLT(X, Y) if ((k = ISLT(X, Y)) < 0) goto fail;  \
           if (k)

/* The maximum number of entries in a MergeState's pending-runs stack.
 * For a list with n elements, this needs at most floor(log2(n)) + 1 entries
 * even if we didn't force runs to a minimal length.  So the number of bits
 * in a ptrdiff_t is plenty large enough for all cases.
 */
#define MAX_MERGE_PENDING (sizeof(size_t) * 8)

/* When we get into galloping mode, we stay there until both runs win less
 * often than MIN_GALLOP consecutive times.  See listsort.txt for more info.
 */
#define MIN_GALLOP 7

/* Avoid malloc for small temp arrays. */
#define MERGESTATE_TEMP_SIZE 256

/* The largest value of minrun. This must be a power of 2, and >= 1, so that
 * the compute_minrun() algorithm guarantees to return a result no larger than
 * this,
 */
#define MAX_MINRUN 64
#if ((MAX_MINRUN) < 1) || ((MAX_MINRUN) & ((MAX_MINRUN) - 1))
#error "MAX_MINRUN must be a power of 2, and >= 1"
#endif

/* One MergeState exists on the stack per invocation of mergesort.  It's just
 * a convenient way to pass state around among the helper functions.
 */
struct s_slice {
    sortslice base;
    ptrdiff_t len;   /* length of run */
    int power; /* node "level" for powersort merge strategy */
};

struct MergeState {
    /* This controls when we get *into* galloping mode.  It's initialized
     * to MIN_GALLOP.  merge_lo and merge_hi tend to nudge it higher for
     * random data, and lower for highly structured data.
     */
    ptrdiff_t min_gallop;

    ptrdiff_t listlen;     /* len(input_list) - read only */
    Item* basekeys;    /* base address of keys array - read only */

    /* 'a' is temp storage to help with merges.  It contains room for
     * alloced entries.
     */
    sortslice a;        /* may point to temparray below */
    ptrdiff_t alloced;

    /* A stack of n pending runs yet to be merged.  Run #i starts at
     * address base[i] and extends for len[i] elements.  It's always
     * true (so long as the indices are in bounds) that
     *
     *     pending[i].base + pending[i].len == pending[i+1].base
     *
     * so we could cut the storage for this, but it's a minor amount,
     * and keeping all the info explicit simplifies the code.
     */
    int n;
    s_slice pending[MAX_MERGE_PENDING];

    /* 'a' points to this when possible, rather than muck with malloc. */
    Item temparray[MERGESTATE_TEMP_SIZE];

    /* This is the function we will use to compare two keys,
     * even when none of our special cases apply and we have to use
     * safe_object_compare. */
    int (*key_compare)(Item , Item , MergeState *);

    /* This function is used by unsafe_tuple_compare to compare the first elements
     * of tuples. It may be set to safe_object_compare, but the idea is that hopefully
     * we can assume more, and use one of the special-case compares. */
    int (*tuple_elem_compare)(Item , Item , MergeState *);

    /* Not in CPython: for the three above. */
    void *context;
    /* Not in CPython: what PyErr_NoMemory() is here. */
    void (*no_memory)(MergeState *);
};

/* binarysort is the best method for sorting small arrays: it does few
   compares, but can do data movement quadratic in the number of elements.
   ss->keys is viewed as an array of n kays, a[:n]. a[:ok] is already sorted.
   Pass ok = 0 (or 1) if you don't know.
   It's sorted in-place, by a stable binary insertion sort. If ss->values
   isn't nullptr, it's permuted in lockstap with ss->keys.
   On entry, must have n >= 1, and 0 <= ok <= n <= MAX_MINRUN.
   Return -1 if comparison raises an exception, else 0.
   Even in case of error, the output slice will be some permutation of
   the input (nothing is lost or duplicated).
*/
static int
binarysort(MergeState *ms, const sortslice *ss, ptrdiff_t n, ptrdiff_t ok)
{
    ptrdiff_t k; /* for IFLT macro expansion */
    Item* const a = ss->keys;
    Item* const v = ss->values;
    const bool has_values = v != nullptr;
    Item pivot;
    ptrdiff_t M;

    ASSERT(0 <= ok && ok <= n && 1 <= n && n <= MAX_MINRUN);
    /* assert a[:ok] is sorted */
    if (! ok)
        ++ok;
    /* Regular insertion sort has average- and worst-case O(n**2) cost
       for both # of comparisons and number of bytes moved. But its branches
       are highly predictable, and it loves sorted input (n-1 compares and no
       data movement). This is significant in cases like sortperf.py's %sort,
       where an out-of-order element near the start of a run is moved into
       place slowly but then the remaining elements up to length minrun are
       generally at worst one slot away from their correct position (so only
       need 1 or 2 commpares to resolve). If comparisons are very fast (such
       as for a list of Python floats), the simple inner loop leaves it
       very competitive with binary insertion, despite that it does
       significantly more compares overall on random data.

       Binary insertion sort has worst, average, and best case O(n log n)
       cost for # of comparisons, but worst and average case O(n**2) cost
       for data movement. The more expensive comparisons, the more important
       the comparison advantage. But its branches are less predictable the
       more "randomish" the data, and that's so significant its worst case
       in real life is random input rather than reverse-ordered (which does
       about twice the data movement than random input does).

       Note that the number of bytes moved doesn't seem to matter. MAX_MINRUN
       of 64 is so small that the key and value pointers all fit in a corner
       of L1 cache, and moving things around in that is very fast. */
#if 0 // ordinary insertion sort.
    Item vpivot = nullptr;
    for (; ok < n; ++ok) {
        pivot = a[ok];
        if (has_values)
            vpivot = v[ok];
        for (M = ok - 1; M >= 0; --M) {
            k = ISLT(pivot, a[M]);
            if (k < 0) {
                a[M + 1] = pivot;
                if (has_values)
                    v[M + 1] = vpivot;
                goto fail;
            }
            else if (k) {
                a[M + 1] = a[M];
                if (has_values)
                    v[M + 1] = v[M];
            }
            else
                break;
        }
        a[M + 1] = pivot;
        if (has_values)
            v[M + 1] = vpivot;
    }
#else // binary insertion sort
    ptrdiff_t L, R;
    for (; ok < n; ++ok) {
        /* set L to where a[ok] belongs */
        L = 0;
        R = ok;
        pivot = a[ok];
        /* Slice invariants. vacuously true at the start:
         * all a[0:L]  <= pivot
         * all a[L:R]     unknown
         * all a[R:ok]  > pivot
         */
        ASSERT(L < R);
        do {
            /* don't do silly ;-) things to prevent overflow when finding
               the midpoint; L and R are very far from filling a ptrdiff_t */
            M = (L + R) >> 1;
#if 1 // straightforward, but highly unpredictable branch on random data
            IFLT(pivot, a[M])
                R = M;
            else
                L = M + 1;
#else
            /* Try to get compiler to generate conditional move instructions
               instead. Works fine, but leaving it disabled for now because
               it's not yielding consistently faster sorts. Needs more
               investigation. More computation in the inner loop adds its own
               costs, which can be significant when compares are fast. */
            k = ISLT(pivot, a[M]);
            if (k < 0)
                goto fail;
            ptrdiff_t Mp1 = M + 1;
            R = k ? M : R;
            L = k ? L : Mp1;
#endif
        } while (L < R);
        ASSERT(L == R);
        /* a[:L] holds all elements from a[:ok] <= pivot now, so pivot belongs
           at index L. Slide a[L:ok] to the right a slot to make room for it.
           Caution: using memmove is much slower under MSVC 5; we're not
           usually moving many slots. Years later: under Visual Studio 2022,
           memmove seems just slightly slower than doing it "by hand". */
        for (M = ok; M > L; --M)
            a[M] = a[M - 1];
        a[L] = pivot;
        if (has_values) {
            pivot = v[ok];
            for (M = ok; M > L; --M)
                v[M] = v[M - 1];
            v[L] = pivot;
        }
    }
#endif // pick binary or regular insertion sort
    return 0;

 fail:
    return -1;
}

static void
sortslice_reverse(sortslice *s, ptrdiff_t n)
{
    reverse_slice(s->keys, &s->keys[n]);
    if (s->values != nullptr)
        reverse_slice(s->values, &s->values[n]);
}

/*
Return the length of the run beginning at slo->keys, spanning no more than
nremaining elements. The run beginning there may be ascending or descending,
but the function permutes it in place, if needed, so that it's always ascending
upon return.

Returns -1 in case of error.
*/
static ptrdiff_t
count_run(MergeState *ms, sortslice *slo, ptrdiff_t nremaining)
{
    ptrdiff_t k; /* used by IFLT macro expansion */
    ptrdiff_t n;
    Item* const lo = slo->keys;

    /* In general, as things go on we've established that the slice starts
       with a monotone run of n elements, starting at lo. */

    /* We're n elements into the slice, and the most recent neq+1 elements are
     * all equal. This reverses them in-place, and resets neq for reuse.
     */
#define REVERSE_LAST_NEQ                        \
    if (neq) {                                  \
        sortslice slice = *slo;                 \
        ++neq;                                  \
        sortslice_advance(&slice, n - neq);     \
        sortslice_reverse(&slice, neq);         \
        neq = 0;                                \
    }

    /* Sticking to only __lt__ compares is confusing and error-prone. But in
     * this routine, almost all uses of IFLT can be captured by tiny macros
     * giving mnemonic names to the intent. Note that inline functions don't
     * work for this (IFLT expands to code including `goto fail`).
     */
#define IF_NEXT_LARGER  IFLT(lo[n-1], lo[n])
#define IF_NEXT_SMALLER IFLT(lo[n], lo[n-1])

    ASSERT(nremaining);
    /* try ascending run first */
    for (n = 1; n < nremaining; ++n) {
        IF_NEXT_SMALLER
            break;
    }
    if (n == nremaining)
        return n;
    /* lo[n] is strictly less */
    /* If n is 1 now, then the first compare established it's a descending
     * run, so fall through to the descending case. But if n > 1, there are
     * n elements in an ascending run terminated by the strictly less lo[n].
     * If the first key < lo[n-1], *somewhere* along the way the sequence
     * increased, so we're done (there is no descending run).
     * Else first key >= lo[n-1], which implies that the entire ascending run
     * consists of equal elements. In that case, this is a descending run,
     * and we reverse the all-equal prefix in-place.
     */
    if (n > 1) {
        IFLT(lo[0], lo[n-1])
            return n;
        sortslice_reverse(slo, n);
    }
    ++n; /* in all cases it's been established that lo[n] has been resolved */

    /* Finish descending run. All-squal subruns are reversed in-place on the
     * fly. Their original order will be restored at the end by the whole-slice
     * reversal.
     */
    ptrdiff_t neq;
    neq = 0;
    for ( ; n < nremaining; ++n) {
        IF_NEXT_SMALLER {
            /* This ends the most recent run of equal elements, but still in
             * the "descending" direction.
             */
            REVERSE_LAST_NEQ
        }
        else {
            IF_NEXT_LARGER /* descending run is over */
                break;
            else /* not x < y and not y < x implies x == y */
                ++neq;
        }
    }
    REVERSE_LAST_NEQ
    sortslice_reverse(slo, n); /* transform to ascending run */

    /* And after reversing, it's possible this can be extended by a
     * naturally increasing suffix; e.g., [3, 2, 3, 4, 1] makes an
     * ascending run from the first 4 elements.
     */
    for ( ; n < nremaining; ++n) {
        IF_NEXT_SMALLER
            break;
    }

    return n;
fail:
    return -1;

#undef REVERSE_LAST_NEQ
#undef IF_NEXT_SMALLER
#undef IF_NEXT_LARGER
}

/*
Locate the proper position of key in a sorted vector; if the vector contains
an element equal to key, return the position immediately to the left of
the leftmost equal element.  [gallop_right() does the same except returns
the position to the right of the rightmost equal element (if any).]

"a" is a sorted vector with n elements, starting at a[0].  n must be > 0.

"hint" is an index at which to begin the search, 0 <= hint < n.  The closer
hint is to the final result, the faster this runs.

The return value is the int k in 0..n such that

    a[k-1] < key <= a[k]

pretending that *(a-1) is minus infinity and a[n] is plus infinity.  IOW,
key belongs at index k; or, IOW, the first k elements of a should precede
key, and the last n-k should follow key.

Returns -1 on error.  See listsort.txt for info on the method.
*/
static ptrdiff_t
gallop_left(MergeState *ms, Item key, Item* a, ptrdiff_t n, ptrdiff_t hint)
{
    ptrdiff_t ofs;
    ptrdiff_t lastofs;
    ptrdiff_t k;

    ASSERT(key && a && n > 0 && hint >= 0 && hint < n);

    a += hint;
    lastofs = 0;
    ofs = 1;
    IFLT(*a, key) {
        /* a[hint] < key -- gallop right, until
         * a[hint + lastofs] < key <= a[hint + ofs]
         */
        const ptrdiff_t maxofs = n - hint;             /* &a[n-1] is highest */
        while (ofs < maxofs) {
            IFLT(a[ofs], key) {
                lastofs = ofs;
                ASSERT(ofs <= (std::numeric_limits<ptrdiff_t>::max() - 1) / 2);
                ofs = (ofs << 1) + 1;
            }
            else                /* key <= a[hint + ofs] */
                break;
        }
        if (ofs > maxofs)
            ofs = maxofs;
        /* Translate back to offsets relative to &a[0]. */
        lastofs += hint;
        ofs += hint;
    }
    else {
        /* key <= a[hint] -- gallop left, until
         * a[hint - ofs] < key <= a[hint - lastofs]
         */
        const ptrdiff_t maxofs = hint + 1;             /* &a[0] is lowest */
        while (ofs < maxofs) {
            IFLT(*(a-ofs), key)
                break;
            /* key <= a[hint - ofs] */
            lastofs = ofs;
            ASSERT(ofs <= (std::numeric_limits<ptrdiff_t>::max() - 1) / 2);
            ofs = (ofs << 1) + 1;
        }
        if (ofs > maxofs)
            ofs = maxofs;
        /* Translate back to positive offsets relative to &a[0]. */
        k = lastofs;
        lastofs = hint - ofs;
        ofs = hint - k;
    }
    a -= hint;

    ASSERT(-1 <= lastofs && lastofs < ofs && ofs <= n);
    /* Now a[lastofs] < key <= a[ofs], so key belongs somewhere to the
     * right of lastofs but no farther right than ofs.  Do a binary
     * search, with invariant a[lastofs-1] < key <= a[ofs].
     */
    ++lastofs;
    while (lastofs < ofs) {
        ptrdiff_t m = lastofs + ((ofs - lastofs) >> 1);

        IFLT(a[m], key)
            lastofs = m+1;              /* a[m] < key */
        else
            ofs = m;                    /* key <= a[m] */
    }
    ASSERT(lastofs == ofs);             /* so a[ofs-1] < key <= a[ofs] */
    return ofs;

fail:
    return -1;
}

/*
Exactly like gallop_left(), except that if key already exists in a[0:n],
finds the position immediately to the right of the rightmost equal value.

The return value is the int k in 0..n such that

    a[k-1] <= key < a[k]

or -1 if error.

The code duplication is massive, but this is enough different given that
we're sticking to "<" comparisons that it's much harder to follow if
written as one routine with yet another "left or right?" flag.
*/
static ptrdiff_t
gallop_right(MergeState *ms, Item key, Item* a, ptrdiff_t n, ptrdiff_t hint)
{
    ptrdiff_t ofs;
    ptrdiff_t lastofs;
    ptrdiff_t k;

    ASSERT(key && a && n > 0 && hint >= 0 && hint < n);

    a += hint;
    lastofs = 0;
    ofs = 1;
    IFLT(key, *a) {
        /* key < a[hint] -- gallop left, until
         * a[hint - ofs] <= key < a[hint - lastofs]
         */
        const ptrdiff_t maxofs = hint + 1;             /* &a[0] is lowest */
        while (ofs < maxofs) {
            IFLT(key, *(a-ofs)) {
                lastofs = ofs;
                ASSERT(ofs <= (std::numeric_limits<ptrdiff_t>::max() - 1) / 2);
                ofs = (ofs << 1) + 1;
            }
            else                /* a[hint - ofs] <= key */
                break;
        }
        if (ofs > maxofs)
            ofs = maxofs;
        /* Translate back to positive offsets relative to &a[0]. */
        k = lastofs;
        lastofs = hint - ofs;
        ofs = hint - k;
    }
    else {
        /* a[hint] <= key -- gallop right, until
         * a[hint + lastofs] <= key < a[hint + ofs]
        */
        const ptrdiff_t maxofs = n - hint;             /* &a[n-1] is highest */
        while (ofs < maxofs) {
            IFLT(key, a[ofs])
                break;
            /* a[hint + ofs] <= key */
            lastofs = ofs;
            ASSERT(ofs <= (std::numeric_limits<ptrdiff_t>::max() - 1) / 2);
            ofs = (ofs << 1) + 1;
        }
        if (ofs > maxofs)
            ofs = maxofs;
        /* Translate back to offsets relative to &a[0]. */
        lastofs += hint;
        ofs += hint;
    }
    a -= hint;

    ASSERT(-1 <= lastofs && lastofs < ofs && ofs <= n);
    /* Now a[lastofs] <= key < a[ofs], so key belongs somewhere to the
     * right of lastofs but no farther right than ofs.  Do a binary
     * search, with invariant a[lastofs-1] <= key < a[ofs].
     */
    ++lastofs;
    while (lastofs < ofs) {
        ptrdiff_t m = lastofs + ((ofs - lastofs) >> 1);

        IFLT(key, a[m])
            ofs = m;                    /* key < a[m] */
        else
            lastofs = m+1;              /* a[m] <= key */
    }
    ASSERT(lastofs == ofs);             /* so a[ofs-1] <= key < a[ofs] */
    return ofs;

fail:
    return -1;
}

/* Conceptually a MergeState's constructor. */
static void
merge_init(MergeState *ms, ptrdiff_t list_size, int has_keyfunc,
           sortslice *lo)
{
    ASSERT(ms != nullptr);
    if (has_keyfunc) {
        /* The temporary space for merging will need at most half the list
         * size rounded up.  Use the minimum possible space so we can use the
         * rest of temparray for other things.  In particular, if there is
         * enough extra space, listsort() will use it to store the keys.
         */
        ms->alloced = (list_size + 1) / 2;

        /* ms->alloced describes how many keys will be stored at
           ms->temparray, but we also need to store the values.  Hence,
           ms->alloced is capped at half of MERGESTATE_TEMP_SIZE. */
        if (MERGESTATE_TEMP_SIZE / 2 < ms->alloced)
            ms->alloced = MERGESTATE_TEMP_SIZE / 2;
        ms->a.values = &ms->temparray[ms->alloced];
    }
    else {
        ms->alloced = MERGESTATE_TEMP_SIZE;
        ms->a.values = nullptr;
    }
    ms->a.keys = ms->temparray;
    ms->n = 0;
    ms->min_gallop = MIN_GALLOP;
    ms->listlen = list_size;
    ms->basekeys = lo->keys;
}

/* Free all the temp memory owned by the MergeState.  This must be called
 * when you're done with a MergeState, and may be called before then if
 * you want to free the temp memory early.
 */
static void
merge_freemem(MergeState *ms)
{
    ASSERT(ms != nullptr);
    if (ms->a.keys != ms->temparray) {
        release(ms->a.keys);
        ms->a.keys = nullptr;
    }
}

/* Ensure enough temp memory for 'need' array slots is available.
 * Returns 0 on success and -1 if the memory can't be gotten.
 */
static int
merge_getmem(MergeState *ms, ptrdiff_t need)
{
    int multiplier;

    ASSERT(ms != nullptr);
    if (need <= ms->alloced)
        return 0;

    multiplier = ms->a.values != nullptr ? 2 : 1;

    /* Don't realloc!  That can cost cycles to copy the old data, but
     * we don't care what's in the block.
     */
    merge_freemem(ms);
    if ((size_t)need > std::numeric_limits<ptrdiff_t>::max() / sizeof(Item) / multiplier) {
        ms->no_memory(ms);
        return -1;
    }
    ms->a.keys = static_cast<Item*>(tryAllocate(multiplier * need
                                          * sizeof(Item)));
    if (ms->a.keys != nullptr) {
        ms->alloced = need;
        if (ms->a.values != nullptr)
            ms->a.values = &ms->a.keys[need];
        return 0;
    }
    ms->no_memory(ms);
    return -1;
}
#define MERGE_GETMEM(MS, NEED) ((NEED) <= (MS)->alloced ? 0 :   \
                                merge_getmem(MS, NEED))

/* Merge the na elements starting at ssa with the nb elements starting at
 * ssb.keys = ssa.keys + na in a stable way, in-place.  na and nb must be > 0.
 * Must also have that ssa.keys[na-1] belongs at the end of the merge, and
 * should have na <= nb.  See listsort.txt for more info.  Return 0 if
 * successful, -1 if error.
 */
static ptrdiff_t
merge_lo(MergeState *ms, sortslice ssa, ptrdiff_t na,
         sortslice ssb, ptrdiff_t nb)
{
    ptrdiff_t k;
    sortslice dest;
    int result = -1;            /* guilty until proved innocent */
    ptrdiff_t min_gallop;

    ASSERT(ms && ssa.keys && ssb.keys && na > 0 && nb > 0);
    ASSERT(ssa.keys + na == ssb.keys);
    if (MERGE_GETMEM(ms, na) < 0)
        return -1;
    sortslice_memcpy(&ms->a, 0, &ssa, 0, na);
    dest = ssa;
    ssa = ms->a;

    sortslice_copy_incr(&dest, &ssb);
    --nb;
    if (nb == 0)
        goto Succeed;
    if (na == 1)
        goto CopyB;

    min_gallop = ms->min_gallop;
    for (;;) {
        ptrdiff_t acount = 0;          /* # of times A won in a row */
        ptrdiff_t bcount = 0;          /* # of times B won in a row */

        /* Do the straightforward thing until (if ever) one run
         * appears to win consistently.
         */
        for (;;) {
            ASSERT(na > 1 && nb > 0);
            k = ISLT(ssb.keys[0], ssa.keys[0]);
            if (k) {
                if (k < 0)
                    goto Fail;
                sortslice_copy_incr(&dest, &ssb);
                ++bcount;
                acount = 0;
                --nb;
                if (nb == 0)
                    goto Succeed;
                if (bcount >= min_gallop)
                    break;
            }
            else {
                sortslice_copy_incr(&dest, &ssa);
                ++acount;
                bcount = 0;
                --na;
                if (na == 1)
                    goto CopyB;
                if (acount >= min_gallop)
                    break;
            }
        }

        /* One run is winning so consistently that galloping may
         * be a huge win.  So try that, and continue galloping until
         * (if ever) neither run appears to be winning consistently
         * anymore.
         */
        ++min_gallop;
        do {
            ASSERT(na > 1 && nb > 0);
            min_gallop -= min_gallop > 1;
            ms->min_gallop = min_gallop;
            k = gallop_right(ms, ssb.keys[0], ssa.keys, na, 0);
            acount = k;
            if (k) {
                if (k < 0)
                    goto Fail;
                sortslice_memcpy(&dest, 0, &ssa, 0, k);
                sortslice_advance(&dest, k);
                sortslice_advance(&ssa, k);
                na -= k;
                if (na == 1)
                    goto CopyB;
                /* na==0 is impossible now if the comparison
                 * function is consistent, but we can't assume
                 * that it is.
                 */
                if (na == 0)
                    goto Succeed;
            }
            sortslice_copy_incr(&dest, &ssb);
            --nb;
            if (nb == 0)
                goto Succeed;

            k = gallop_left(ms, ssa.keys[0], ssb.keys, nb, 0);
            bcount = k;
            if (k) {
                if (k < 0)
                    goto Fail;
                sortslice_memmove(&dest, 0, &ssb, 0, k);
                sortslice_advance(&dest, k);
                sortslice_advance(&ssb, k);
                nb -= k;
                if (nb == 0)
                    goto Succeed;
            }
            sortslice_copy_incr(&dest, &ssa);
            --na;
            if (na == 1)
                goto CopyB;
        } while (acount >= MIN_GALLOP || bcount >= MIN_GALLOP);
        ++min_gallop;           /* penalize it for leaving galloping mode */
        ms->min_gallop = min_gallop;
    }
Succeed:
    result = 0;
Fail:
    if (na)
        sortslice_memcpy(&dest, 0, &ssa, 0, na);
    return result;
CopyB:
    ASSERT(na == 1 && nb > 0);
    /* The last element of ssa belongs at the end of the merge. */
    sortslice_memmove(&dest, 0, &ssb, 0, nb);
    sortslice_copy(&dest, nb, &ssa, 0);
    return 0;
}

/* Merge the na elements starting at pa with the nb elements starting at
 * ssb.keys = ssa.keys + na in a stable way, in-place.  na and nb must be > 0.
 * Must also have that ssa.keys[na-1] belongs at the end of the merge, and
 * should have na >= nb.  See listsort.txt for more info.  Return 0 if
 * successful, -1 if error.
 */
static ptrdiff_t
merge_hi(MergeState *ms, sortslice ssa, ptrdiff_t na,
         sortslice ssb, ptrdiff_t nb)
{
    ptrdiff_t k;
    sortslice dest, basea, baseb;
    int result = -1;            /* guilty until proved innocent */
    ptrdiff_t min_gallop;

    ASSERT(ms && ssa.keys && ssb.keys && na > 0 && nb > 0);
    ASSERT(ssa.keys + na == ssb.keys);
    if (MERGE_GETMEM(ms, nb) < 0)
        return -1;
    dest = ssb;
    sortslice_advance(&dest, nb-1);
    sortslice_memcpy(&ms->a, 0, &ssb, 0, nb);
    basea = ssa;
    baseb = ms->a;
    ssb.keys = ms->a.keys + nb - 1;
    if (ssb.values != nullptr)
        ssb.values = ms->a.values + nb - 1;
    sortslice_advance(&ssa, na - 1);

    sortslice_copy_decr(&dest, &ssa);
    --na;
    if (na == 0)
        goto Succeed;
    if (nb == 1)
        goto CopyA;

    min_gallop = ms->min_gallop;
    for (;;) {
        ptrdiff_t acount = 0;          /* # of times A won in a row */
        ptrdiff_t bcount = 0;          /* # of times B won in a row */

        /* Do the straightforward thing until (if ever) one run
         * appears to win consistently.
         */
        for (;;) {
            ASSERT(na > 0 && nb > 1);
            k = ISLT(ssb.keys[0], ssa.keys[0]);
            if (k) {
                if (k < 0)
                    goto Fail;
                sortslice_copy_decr(&dest, &ssa);
                ++acount;
                bcount = 0;
                --na;
                if (na == 0)
                    goto Succeed;
                if (acount >= min_gallop)
                    break;
            }
            else {
                sortslice_copy_decr(&dest, &ssb);
                ++bcount;
                acount = 0;
                --nb;
                if (nb == 1)
                    goto CopyA;
                if (bcount >= min_gallop)
                    break;
            }
        }

        /* One run is winning so consistently that galloping may
         * be a huge win.  So try that, and continue galloping until
         * (if ever) neither run appears to be winning consistently
         * anymore.
         */
        ++min_gallop;
        do {
            ASSERT(na > 0 && nb > 1);
            min_gallop -= min_gallop > 1;
            ms->min_gallop = min_gallop;
            k = gallop_right(ms, ssb.keys[0], basea.keys, na, na-1);
            if (k < 0)
                goto Fail;
            k = na - k;
            acount = k;
            if (k) {
                sortslice_advance(&dest, -k);
                sortslice_advance(&ssa, -k);
                sortslice_memmove(&dest, 1, &ssa, 1, k);
                na -= k;
                if (na == 0)
                    goto Succeed;
            }
            sortslice_copy_decr(&dest, &ssb);
            --nb;
            if (nb == 1)
                goto CopyA;

            k = gallop_left(ms, ssa.keys[0], baseb.keys, nb, nb-1);
            if (k < 0)
                goto Fail;
            k = nb - k;
            bcount = k;
            if (k) {
                sortslice_advance(&dest, -k);
                sortslice_advance(&ssb, -k);
                sortslice_memcpy(&dest, 1, &ssb, 1, k);
                nb -= k;
                if (nb == 1)
                    goto CopyA;
                /* nb==0 is impossible now if the comparison
                 * function is consistent, but we can't assume
                 * that it is.
                 */
                if (nb == 0)
                    goto Succeed;
            }
            sortslice_copy_decr(&dest, &ssa);
            --na;
            if (na == 0)
                goto Succeed;
        } while (acount >= MIN_GALLOP || bcount >= MIN_GALLOP);
        ++min_gallop;           /* penalize it for leaving galloping mode */
        ms->min_gallop = min_gallop;
    }
Succeed:
    result = 0;
Fail:
    if (nb)
        sortslice_memcpy(&dest, -(nb-1), &baseb, 0, nb);
    return result;
CopyA:
    ASSERT(nb == 1 && na > 0);
    /* The first element of ssb belongs at the front of the merge. */
    sortslice_memmove(&dest, 1-na, &ssa, 1-na, na);
    sortslice_advance(&dest, -na);
    sortslice_advance(&ssa, -na);
    sortslice_copy(&dest, 0, &ssb, 0);
    return 0;
}

/* Merge the two runs at stack indices i and i+1.
 * Returns 0 on success, -1 on error.
 */
static ptrdiff_t
merge_at(MergeState *ms, ptrdiff_t i)
{
    sortslice ssa, ssb;
    ptrdiff_t na, nb;
    ptrdiff_t k;

    ASSERT(ms != nullptr);
    ASSERT(ms->n >= 2);
    ASSERT(i >= 0);
    ASSERT(i == ms->n - 2 || i == ms->n - 3);

    ssa = ms->pending[i].base;
    na = ms->pending[i].len;
    ssb = ms->pending[i+1].base;
    nb = ms->pending[i+1].len;
    ASSERT(na > 0 && nb > 0);
    ASSERT(ssa.keys + na == ssb.keys);

    /* Record the length of the combined runs; if i is the 3rd-last
     * run now, also slide over the last run (which isn't involved
     * in this merge).  The current run i+1 goes away in any case.
     */
    ms->pending[i].len = na + nb;
    if (i == ms->n - 3)
        ms->pending[i+1] = ms->pending[i+2];
    --ms->n;

    /* Where does b start in a?  Elements in a before that can be
     * ignored (already in place).
     */
    k = gallop_right(ms, *ssb.keys, ssa.keys, na, 0);
    if (k < 0)
        return -1;
    sortslice_advance(&ssa, k);
    na -= k;
    if (na == 0)
        return 0;

    /* Where does a end in b?  Elements in b after that can be
     * ignored (already in place).
     */
    nb = gallop_left(ms, ssa.keys[na-1], ssb.keys, nb, nb-1);
    if (nb <= 0)
        return nb;

    /* Merge what remains of the runs, using a temp array with
     * min(na, nb) elements.
     */
    if (na <= nb)
        return merge_lo(ms, ssa, na, ssb, nb);
    else
        return merge_hi(ms, ssa, na, ssb, nb);
}

/* Two adjacent runs begin at index s1. The first run has length n1, and
 * the second run (starting at index s1+n1) has length n2. The list has total
 * length n.
 * Compute the "power" of the first run. See listsort.txt for details.
 */
static int
powerloop(ptrdiff_t s1, ptrdiff_t n1, ptrdiff_t n2, ptrdiff_t n)
{
    int result = 0;
    ASSERT(s1 >= 0);
    ASSERT(n1 > 0 && n2 > 0);
    ASSERT(s1 + n1 + n2 <= n);
    /* midpoints a and b:
     * a = s1 + n1/2
     * b = s1 + n1 + n2/2 = a + (n1 + n2)/2
     *
     * Those may not be integers, though, because of the "/2". So we work with
     * 2*a and 2*b instead, which are necessarily integers. It makes no
     * difference to the outcome, since the bits in the expansion of (2*i)/n
     * are merely shifted one position from those of i/n.
     */
    ptrdiff_t a = 2 * s1 + n1;  /* 2*a */
    ptrdiff_t b = a + n1 + n2;  /* 2*b */
    /* Emulate a/n and b/n one bit a time, until bits differ. */
    for (;;) {
        ++result;
        if (a >= n) {  /* both quotient bits are 1 */
            ASSERT(b >= a);
            a -= n;
            b -= n;
        }
        else if (b >= n) {  /* a/n bit is 0, b/n bit is 1 */
            break;
        } /* else both quotient bits are 0 */
        ASSERT(a < b && b < n);
        a <<= 1;
        b <<= 1;
    }
    return result;
}

/* The next run has been identified, of length n2.
 * If there's already a run on the stack, apply the "powersort" merge strategy:
 * compute the topmost run's "power" (depth in a conceptual binary merge tree)
 * and merge adjacent runs on the stack with greater power. See listsort.txt
 * for more info.
 *
 * It's the caller's responsibility to push the new run on the stack when this
 * returns.
 *
 * Returns 0 on success, -1 on error.
 */
static int
found_new_run(MergeState *ms, ptrdiff_t n2)
{
    ASSERT(ms);
    if (ms->n) {
        ASSERT(ms->n > 0);
        struct s_slice *p = ms->pending;
        ptrdiff_t s1 = p[ms->n - 1].base.keys - ms->basekeys; /* start index */
        ptrdiff_t n1 = p[ms->n - 1].len;
        int power = powerloop(s1, n1, n2, ms->listlen);
        while (ms->n > 1 && p[ms->n - 2].power > power) {
            if (merge_at(ms, ms->n - 2) < 0)
                return -1;
        }
        ASSERT(ms->n < 2 || p[ms->n - 2].power < power);
        p[ms->n - 1].power = power;
    }
    return 0;
}

/* Regardless of invariants, merge all runs on the stack until only one
 * remains.  This is used at the end of the mergesort.
 *
 * Returns 0 on success, -1 on error.
 */
static int
merge_force_collapse(MergeState *ms)
{
    struct s_slice *p = ms->pending;

    ASSERT(ms);
    while (ms->n > 1) {
        ptrdiff_t n = ms->n - 2;
        if (n > 0 && p[n-1].len < p[n+1].len)
            --n;
        if (merge_at(ms, n) < 0)
            return -1;
    }
    return 0;
}

/* Compute a good value for the minimum run length; natural runs shorter
 * than this are boosted artificially via binary insertion.
 *
 * If n < MAX_MINRUN return n (it's too small to bother with fancy stuff).
 * Else if n is an exact power of 2, return MAX_MINRUN / 2.
 * Else return an int k, MAX_MINRUN / 2 <= k <= MAX_MINRUN, such that n/k is
 * close to, but strictly less than, an exact power of 2.
 *
 * See listsort.txt for more info.
 */
static ptrdiff_t
merge_compute_minrun(ptrdiff_t n)
{
    ptrdiff_t r = 0;           /* becomes 1 if any 1 bits are shifted off */

    ASSERT(n >= 0);
    while (n >= MAX_MINRUN) {
        r |= n & 1;
        n >>= 1;
    }
    return n + r;
}

/* Here we define custom comparison functions to optimize for the cases one commonly
 * encounters in practice: homogeneous lists, often of one of the basic types. */

#undef ISLT
#undef IFLT
#undef MAX_MERGE_PENDING
#undef MIN_GALLOP
#undef MERGESTATE_TEMP_SIZE
#undef MAX_MINRUN
#undef MERGE_GETMEM

} } } // namespace JSC::Python::ListSortKernel

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
