#include "roaringbitmap.h"
#include "utils/lsyscache.h"

/* Created by ZEROMAX on 2017/3/20.*/

#define MAX_BITMAP_RANGE_END UINT64_C(0x100000000)
#define INT4_MIN -2147483648
#define INT4_MAX 2147483647

/* GUC variables */

typedef enum
{
    RBITMAP_OUTPUT_ARRAY,                /* output as int array */
    RBITMAP_OUTPUT_BYTEA                 /* output as bytea */
}RBITMAPOutputFormat;

static const struct config_enum_entry output_format_options[] =
{
    {"array", RBITMAP_OUTPUT_ARRAY, false},
    {"bytea", RBITMAP_OUTPUT_BYTEA, false},
    {NULL, 0, false}
};

static int    rbitmap_output_format = RBITMAP_OUTPUT_BYTEA;        /* output format */

static roaring_memory_t rb_memory_hook = {
    .malloc = palloc,
    .realloc = pg_realloc,
    .calloc = pg_calloc,
    .free = pg_free,
    .aligned_malloc = pg_aligned_malloc,
    .aligned_free = pg_aligned_free,
};

void        _PG_init(void);
/*
 * Module load callback
 */
void
_PG_init(void)
{
    /* Define custom GUC variables. */
    DefineCustomEnumVariable("roaringbitmap.output_format",
                             "Selects output format of roaringbitmap.",
                             NULL,
                             &rbitmap_output_format,
                             RBITMAP_OUTPUT_BYTEA,
                             output_format_options,
                             PGC_USERSET,
                             0,
                             NULL,
                             NULL,
                             NULL);
    roaring_init_memory_hook(rb_memory_hook);
}


bool
ArrayContainsNulls(ArrayType *array) {
    int nelems;
    bits8 *bitmap;
    int bitmask;

    /* Easy answer if there's no null bitmap */
    if (!ARR_HASNULL(array))
        return false;

    nelems = ArrayGetNItems(ARR_NDIM(array), ARR_DIMS(array));

    bitmap = ARR_NULLBITMAP(array);

    /* check whole bytes of the bitmap byte-at-a-time */
    while (nelems >= 8) {
        if (*bitmap != 0xFF)
            return true;
        bitmap++;
        nelems -= 8;
    }

    /* check last partial byte */
    bitmask = 1;
    while (nelems > 0) {
        if ((*bitmap & bitmask) == 0)
            return true;
        bitmask <<= 1;
        nelems--;
    }

    return false;
}



// rb_kmerge SRF
PG_FUNCTION_INFO_V1(rb_kmerge);
Datum rb_kmerge(PG_FUNCTION_ARGS);

/* ============================================================
 * rb_kmerge: k-way merge with source-set grouping
 * ============================================================
 *
 * Given N bitmaps, performs a k-way merge and groups elements by which
 * combination of input bitmaps contains them.  Returns one row per unique
 * source-set pattern with:
 *   sources int[]         – 1-based indices of input bitmaps
 *   members roaringbitmap – all elements sharing that source-set
 *
 * Uses PostgreSQL's lib/simplehash.h for the hash table and a hand-rolled
 * min-heap for the k-way merge.
 */

#define KM_MAX_WORDS 16  /* supports up to 1024 inputs */

/* --- Min-heap for k-way merge --- */

typedef struct KMHeapNode
{
    int     src;    /* 0-based index of iterator */
    uint32  value;  /* iterator's current value */
} KMHeapNode;

static inline void
km_heap_sift_down(KMHeapNode *heap, int size, int idx)
{
    for (;;)
    {
        int left = (idx << 1) + 1;
        if (left >= size) break;
        int right = left + 1;
        int smallest = left;
        if (right < size && heap[right].value < heap[left].value)
            smallest = right;
        if (!(heap[smallest].value < heap[idx].value))
            break;
        KMHeapNode tmp = heap[idx];
        heap[idx] = heap[smallest];
        heap[smallest] = tmp;
        idx = smallest;
    }
}

static inline void
km_heap_build(KMHeapNode *heap, int size)
{
    for (int i = (size >> 1) - 1; i >= 0; i--)
        km_heap_sift_down(heap, size, i);
}

/* --- Source-set key: variable-width bitmask in a fixed-max struct --- */

typedef struct KMGroupKey
{
    uint64 words[KM_MAX_WORDS];
} KMGroupKey;

/* Hash a source-set key (only the first nwords words matter) */
static inline uint32
kmg_hash_key(const uint64 *words, int nwords)
{
    uint64 h = 0;
    for (int i = 0; i < nwords; i++)
    {
        h ^= words[i];
        h ^= h >> 30;
        h *= 0xbf58476d1ce4e5b9ULL;
        h ^= h >> 27;
        h *= 0x94d049bb133111ebULL;
        h ^= h >> 31;
    }
    return (uint32) h;
}

/* Private data threaded through the hash table for variable-width ops */
typedef struct KMGroupPrivate
{
    int nwords;
} KMGroupPrivate;

/* --- simplehash element type --- */

typedef struct KMGroupEntry
{
    KMGroupKey              key;
    roaring_bitmap_t       *members;
    roaring_bulk_context_t  bulk_ctx;
    char                    status;     /* required by simplehash */
} KMGroupEntry;

/* Instantiate the hash table (declarations) */
#define SH_PREFIX           kmgroup
#define SH_ELEMENT_TYPE     KMGroupEntry
#define SH_KEY_TYPE         KMGroupKey
#define SH_KEY              key
#define SH_HASH_KEY(tb, k)  kmg_hash_key((k).words, \
                                ((KMGroupPrivate *) (tb)->private_data)->nwords)
#define SH_EQUAL(tb, a, b)  (memcmp((a).words, (b).words, \
                                ((KMGroupPrivate *) (tb)->private_data)->nwords \
                                * sizeof(uint64)) == 0)
#define SH_SCOPE            static inline
#define SH_DECLARE
#define SH_DEFINE
#include "lib/simplehash.h"

/* --- SRF state: hash table + iterator --- */

typedef struct
{
    kmgroup_hash       *ht;
    kmgroup_iterator    iter;
    int                 nwords;
    TupleDesc           tupdesc;
} KMergeState;

Datum
rb_kmerge(PG_FUNCTION_ARGS)
{
    FuncCallContext    *funcctx;
    MemoryContext       oldcontext;

    if (SRF_IS_FIRSTCALL())
    {
        ArrayType  *arr = PG_GETARG_ARRAYTYPE_P(0);
        funcctx = SRF_FIRSTCALL_INIT();
        oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

        /* Deconstruct input array */
        int16   elmlen;
        bool    elmbyval;
        char    elmalign;
        Oid     elmtype = ARR_ELEMTYPE(arr);
        get_typlenbyvalalign(elmtype, &elmlen, &elmbyval, &elmalign);

        Datum  *elem_values;
        bool   *elem_nulls;
        int     nelems;
        deconstruct_array(arr, elmtype, elmlen, elmbyval, elmalign,
                          &elem_values, &elem_nulls, &nelems);

        /* Count non-NULL bitmaps */
        int count = 0;
        for (int i = 0; i < nelems; i++)
        {
            if (!elem_nulls[i])
                count++;
        }

        int nwords = (count + 63) / 64;
        if (nwords < 1) nwords = 1;
        if (nwords > KM_MAX_WORDS)
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("rb_kmerge supports at most %d inputs",
                            KM_MAX_WORDS * 64)));

        /* Deserialize bitmaps and create iterators */
        roaring_uint32_iterator_t **iters =
            (roaring_uint32_iterator_t **) palloc0(
                sizeof(roaring_uint32_iterator_t *) * Max(count, 1));

        KMHeapNode *heap = (KMHeapNode *) palloc(sizeof(KMHeapNode) * Max(count, 1));
        int heap_size = 0;
        int out_idx = 0;

        for (int i = 0; i < nelems; i++)
        {
            if (elem_nulls[i])
                continue;

            bytea *data = (bytea *) DatumGetPointer(elem_values[i]);
            roaring_bitmap_t *rb =
                roaring_bitmap_portable_deserialize(VARDATA(data));
            if (!rb)
                ereport(ERROR,
                        (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                         errmsg("bitmap format is error")));

            roaring_uint32_iterator_t *it = roaring_iterator_create(rb);
            iters[out_idx] = it;
            if (it->has_value)
            {
                heap[heap_size].src = out_idx;
                heap[heap_size].value = it->current_value;
                heap_size++;
            }
            out_idx++;
        }
        if (heap_size > 1)
            km_heap_build(heap, heap_size);

        /* Set up hash table for grouping by source-set */
        KMGroupPrivate *priv =
            (KMGroupPrivate *) palloc(sizeof(KMGroupPrivate));
        priv->nwords = nwords;
        kmgroup_hash *ht = kmgroup_create(funcctx->multi_call_memory_ctx,
                                          256, priv);

        /* K-way merge: collect sources for each element, group in HT */
        KMGroupKey bitmask;
        memset(&bitmask, 0, sizeof(bitmask));

        while (heap_size > 0)
        {
            uint32 current_val = heap[0].value;
            memset(bitmask.words, 0, nwords * sizeof(uint64));

            /* Collect all sources that contain current_val */
            do
            {
                int src = heap[0].src;
                bitmask.words[src / 64] |= ((uint64) 1) << (src % 64);

                roaring_uint32_iterator_t *it = iters[src];
                roaring_uint32_iterator_advance(it);
                if (it->has_value)
                {
                    heap[0].value = it->current_value;
                    km_heap_sift_down(heap, heap_size, 0);
                }
                else
                {
                    heap[0] = heap[heap_size - 1];
                    heap_size--;
                    if (heap_size > 0)
                        km_heap_sift_down(heap, heap_size, 0);
                }
            }
            while (heap_size > 0 && heap[0].value == current_val);

            /* Insert into the group for this source-set */
            bool found;
            KMGroupEntry *entry = kmgroup_insert(ht, bitmask, &found);
            if (!found)
            {
                entry->members = roaring_bitmap_create();
                memset(&entry->bulk_ctx, 0, sizeof(roaring_bulk_context_t));
            }
            roaring_bitmap_add_bulk(entry->members, &entry->bulk_ctx,
                                    current_val);
        }

        /* Clean up iterators and heap */
        for (int i = 0; i < count; i++)
        {
            if (iters[i])
                roaring_uint32_iterator_free(iters[i]);
        }
        pfree(iters);
        pfree(heap);

        /* Store HT and prepare iterator for per-call phase */
        KMergeState *state =
            (KMergeState *) palloc0(sizeof(KMergeState));
        state->ht = ht;
        state->nwords = nwords;
        kmgroup_start_iterate(ht, &state->iter);

        TupleDesc tupdesc;
        if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
            ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                     errmsg("return type must be a row type")));
        BlessTupleDesc(tupdesc);
        state->tupdesc = tupdesc;

        funcctx->user_fctx = state;
        MemoryContextSwitchTo(oldcontext);
    }

    funcctx = SRF_PERCALL_SETUP();
    KMergeState *state = (KMergeState *) funcctx->user_fctx;

    KMGroupEntry *entry = kmgroup_iterate(state->ht, &state->iter);
    if (entry == NULL)
        SRF_RETURN_DONE(funcctx);

    int nwords = state->nwords;

    /* Convert bitmask to int[] of 1-based source indices */
    int nsources = 0;
    for (int w = 0; w < nwords; w++)
    {
        uint64 v = entry->key.words[w];
        while (v) { nsources++; v &= v - 1; }
    }

    Datum *src_datums = (Datum *) palloc(sizeof(Datum) * nsources);
    int sidx = 0;
    for (int w = 0; w < nwords; w++)
    {
        uint64 v = entry->key.words[w];
        int base = w * 64;
        while (v)
        {
            int bit = __builtin_ctzll(v);
            src_datums[sidx++] = Int32GetDatum(base + bit + 1);
            v &= v - 1;
        }
    }
    ArrayType *src_array = construct_array(src_datums, nsources, INT4OID,
                                           sizeof(int32), true, 'i');

    /* Serialize the members bitmap */
    size_t portable_size = roaring_bitmap_portable_size_in_bytes(entry->members);
    bytea *serialized = (bytea *) palloc(VARHDRSZ + portable_size);
    roaring_bitmap_portable_serialize(entry->members, VARDATA(serialized));
    SET_VARSIZE(serialized, VARHDRSZ + portable_size);

    Datum vals[2];
    bool nulls[2] = {false, false};
    vals[0] = PointerGetDatum(src_array);
    vals[1] = PointerGetDatum(serialized);

    HeapTuple tuple = heap_form_tuple(state->tupdesc, vals, nulls);
    SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
}

//rb_from_bytea
Datum rb_from_bytea(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(rb_from_bytea);

Datum
rb_from_bytea(PG_FUNCTION_ARGS) {
    bytea *serializedbytes = PG_GETARG_BYTEA_P(0);
    roaring_bitmap_t *r1;
    size_t expectedsize;
    bytea *serializedbytes2;

    r1 = roaring_bitmap_portable_deserialize_safe(VARDATA(serializedbytes), VARSIZE(serializedbytes) - VARHDRSZ);
    if (!r1)
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));

    expectedsize = roaring_bitmap_portable_size_in_bytes(r1);
    serializedbytes2 = (bytea *) palloc(VARHDRSZ + expectedsize);
    roaring_bitmap_portable_serialize(r1, VARDATA(serializedbytes2));
    roaring_bitmap_free(r1);

    SET_VARSIZE(serializedbytes2, VARHDRSZ + expectedsize);
    PG_RETURN_BYTEA_P(serializedbytes2);
}


// rb_deserialize_native
Datum rb_deserialize_native(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(rb_deserialize_native);

Datum
rb_deserialize_native(PG_FUNCTION_ARGS) {
    bytea *input = PG_GETARG_BYTEA_P(0);
    roaring_bitmap_t *bitmap;
    size_t portable_size;
    bytea *result;

    bitmap = roaring_bitmap_deserialize_safe(VARDATA(input), VARSIZE(input) - VARHDRSZ);
    if (!bitmap)
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));

    portable_size = roaring_bitmap_portable_size_in_bytes(bitmap);
    result = (bytea *) palloc(VARHDRSZ + portable_size);
    roaring_bitmap_portable_serialize(bitmap, VARDATA(result));
    roaring_bitmap_free(bitmap);

    SET_VARSIZE(result, VARHDRSZ + portable_size);
    PG_RETURN_BYTEA_P(result);
}


//roaringbitmap_in
Datum roaringbitmap_in(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(roaringbitmap_in);

Datum
roaringbitmap_in(PG_FUNCTION_ARGS) {
    char       *ptr = PG_GETARG_CSTRING(0);
    long        l;
    char       *badp;
    roaring_bitmap_t *r1;
    size_t expectedsize;
    bytea *serializedbytes;
    Datum dd;

    if(*ptr == '\\' && *(ptr+1) == 'x') {
       /* bytea input */
        dd = DirectFunctionCall1(byteain, PG_GETARG_DATUM(0));

        serializedbytes = DatumGetByteaP(dd);
        r1 = roaring_bitmap_portable_deserialize_safe(VARDATA(serializedbytes), VARSIZE(serializedbytes) - VARHDRSZ);
        if (!r1)
            ereport(ERROR,
                    (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                     errmsg("bitmap format is error")));

        expectedsize = roaring_bitmap_portable_size_in_bytes(r1);
        serializedbytes = (bytea *) palloc(VARHDRSZ + expectedsize);
        roaring_bitmap_portable_serialize(r1, VARDATA(serializedbytes));
        roaring_bitmap_free(r1);

        SET_VARSIZE(serializedbytes, VARHDRSZ + expectedsize);
        PG_RETURN_BYTEA_P(serializedbytes);
    }
    /* else int array input */

    /* Find the head char '{' */
    while (*ptr && isspace((unsigned char) *ptr))
            ptr++;
    if (*ptr !='{')
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("malformed bitmap literal")));
    ptr++;

    r1 = roaring_bitmap_create();

    while (*ptr && isspace((unsigned char) *ptr))
        ptr++;

    if (*ptr != '}') {
        while (*ptr) {
            /* Parse int element */
            errno = 0;
            l = strtol(ptr, &badp, 10);

            /* We made no progress parsing the string, so bail out */
            if (ptr == badp){
                roaring_bitmap_free(r1);
                ereport(ERROR,
                    (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                     errmsg("invalid input syntax for %s: \"%s\"",
                            "integer", ptr)));
            }

            if (errno == ERANGE
                || l < INT4_MIN || l > INT4_MAX
                ){
                    roaring_bitmap_free(r1);
                    ereport(ERROR,
                            (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                             errmsg("value \"%s\" is out of range for type %s", ptr,
                                    "integer")));
                }

            /* Add int element to bitmap */
            roaring_bitmap_add(r1, l);

            /* Skip any trailing whitespace after the int element */
            ptr = badp;
            while (*ptr && isspace((unsigned char) *ptr))
                ptr++;

            /* Find the element terminator ',' */
            if (*ptr != ',')
                break;
            ptr++;

            /* Skip any trailing whitespace after the terminator */
            while (*ptr && isspace((unsigned char) *ptr))
                ptr++;
        }

        /* Find the tail char '{' */
        if (*ptr !='}'){
            roaring_bitmap_free(r1);
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("malformed bitmap literal")));
        }
    }

    /* Check if input end */
    ptr++;
    while (*ptr && isspace((unsigned char) *ptr))
        ptr++;

    if (*ptr !='\0'){
        roaring_bitmap_free(r1);
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("malformed bitmap literal")));
    }

    expectedsize = roaring_bitmap_portable_size_in_bytes(r1);
    serializedbytes = (bytea *) palloc(VARHDRSZ + expectedsize);
    roaring_bitmap_portable_serialize(r1, VARDATA(serializedbytes));
    roaring_bitmap_free(r1);

    SET_VARSIZE(serializedbytes, VARHDRSZ + expectedsize);
    PG_RETURN_BYTEA_P(serializedbytes);
}

//roaringbitmap_out
Datum roaringbitmap_out(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(roaringbitmap_out);

Datum
roaringbitmap_out(PG_FUNCTION_ARGS) {
    bytea *serializedbytes;
    roaring_uint32_iterator_t iterator;
    StringInfoData buf;
    roaring_bitmap_t *r1;
    
    if(rbitmap_output_format == RBITMAP_OUTPUT_BYTEA){
        return DirectFunctionCall1(byteaout, PG_GETARG_DATUM(0));
    }
    
    serializedbytes = PG_GETARG_BYTEA_P(0);
    r1 = roaring_bitmap_portable_deserialize(VARDATA(serializedbytes));
    if (!r1)
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));

    initStringInfo(&buf);

    appendStringInfoChar(&buf, '{');

    roaring_iterator_init(r1, &iterator);
    if(iterator.has_value) {
        appendStringInfo(&buf, "%d", (int)iterator.current_value);
        roaring_uint32_iterator_advance(&iterator);

        while(iterator.has_value) {
            appendStringInfo(&buf, ",%d", (int)iterator.current_value);
            roaring_uint32_iterator_advance(&iterator);
        }
    }

    appendStringInfoChar(&buf, '}');

    PG_RETURN_CSTRING(buf.data);
}

//roaringbitmap_recv
Datum roaringbitmap_recv(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(roaringbitmap_recv);

Datum
roaringbitmap_recv(PG_FUNCTION_ARGS) {
    return DirectFunctionCall1(bytearecv, PG_GETARG_DATUM(0));
}


//roaringbitmap_send
Datum roaringbitmap_send(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(roaringbitmap_send);

Datum
roaringbitmap_send(PG_FUNCTION_ARGS) {
    bytea *bp = PG_GETARG_BYTEA_P(0);
    StringInfoData buf;

    pq_begintypsend(&buf);
    pq_sendbytes(&buf, VARDATA(bp), VARSIZE(bp) - VARHDRSZ);
    PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}


//bitmap_or
PG_FUNCTION_INFO_V1(rb_or);
Datum rb_or(PG_FUNCTION_ARGS);

Datum
rb_or(PG_FUNCTION_ARGS) {
    bytea *serializedbytes1 = PG_GETARG_BYTEA_P(0);
    bytea *serializedbytes2 = PG_GETARG_BYTEA_P(1);
    roaring_bitmap_t *r1;
    roaring_bitmap_t *r2;
    size_t expectedsize;
    bytea *serializedbytes;

    r1 = roaring_bitmap_portable_deserialize(VARDATA(serializedbytes1));
    if (!r1)
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));

    r2 = roaring_bitmap_portable_deserialize(VARDATA(serializedbytes2));
    if (!r2) {
        roaring_bitmap_free(r1);
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));
    }
    roaring_bitmap_or_inplace(r1, r2);
    roaring_bitmap_free(r2);
    expectedsize = roaring_bitmap_portable_size_in_bytes(r1);
    serializedbytes = (bytea *) palloc(VARHDRSZ + expectedsize);
    roaring_bitmap_portable_serialize(r1, VARDATA(serializedbytes));
    roaring_bitmap_free(r1);

    SET_VARSIZE(serializedbytes, VARHDRSZ + expectedsize);
    PG_RETURN_BYTEA_P(serializedbytes);
}


//bitmap_or_cardinality
PG_FUNCTION_INFO_V1(rb_or_cardinality);
Datum rb_or_cardinality(PG_FUNCTION_ARGS);

Datum
rb_or_cardinality(PG_FUNCTION_ARGS) {
    bytea *serializedbytes1 = PG_GETARG_BYTEA_P(0);
    bytea *serializedbytes2 = PG_GETARG_BYTEA_P(1);
    roaring_buffer_t *r1;
    roaring_buffer_t *r2;
    uint64 card1;
    bool ret;

    r1 = roaring_buffer_create(VARDATA(serializedbytes1),
                               VARSIZE(serializedbytes1));
    if (!r1)
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));

    r2 = roaring_buffer_create(VARDATA(serializedbytes2),
                               VARSIZE(serializedbytes2));
    if (!r2) {
        roaring_buffer_free(r1);
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));
    }

    ret = roaring_buffer_or_cardinality(r1, r2, &card1);
    roaring_buffer_free(r1);
    roaring_buffer_free(r2);
    if(!ret)
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));

    PG_RETURN_INT64(card1);
}

//bitmap_and
PG_FUNCTION_INFO_V1(rb_and);
Datum rb_and(PG_FUNCTION_ARGS);

Datum
rb_and(PG_FUNCTION_ARGS) {
    bytea *serializedbytes1 = PG_GETARG_BYTEA_P(0);
    bytea *serializedbytes2 = PG_GETARG_BYTEA_P(1);
    roaring_buffer_t *r1;
    roaring_buffer_t *r2;
    roaring_bitmap_t *r;
    size_t expectedsize;
    bytea *serializedbytes;

    r1 = roaring_buffer_create(VARDATA(serializedbytes1),
                               VARSIZE(serializedbytes1));
    if (!r1)
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));

    r2 = roaring_buffer_create(VARDATA(serializedbytes2),
                               VARSIZE(serializedbytes2));
    if (!r2) {
        roaring_buffer_free(r1);
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));
    }

    r = roaring_buffer_and(r1, r2);
    roaring_buffer_free(r1);
    roaring_buffer_free(r2);
    if (!r) {
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));
    }

    expectedsize = roaring_bitmap_portable_size_in_bytes(r);
    serializedbytes = (bytea *) palloc(VARHDRSZ + expectedsize);
    roaring_bitmap_portable_serialize(r, VARDATA(serializedbytes));
    roaring_bitmap_free(r);

    SET_VARSIZE(serializedbytes, VARHDRSZ + expectedsize);
    PG_RETURN_BYTEA_P(serializedbytes);
}


//bitmap_and_cardinality
PG_FUNCTION_INFO_V1(rb_and_cardinality);
Datum rb_and_cardinality(PG_FUNCTION_ARGS);

Datum
rb_and_cardinality(PG_FUNCTION_ARGS) {
    bytea *serializedbytes1 = PG_GETARG_BYTEA_P(0);
    bytea *serializedbytes2 = PG_GETARG_BYTEA_P(1);
    roaring_buffer_t *r1;
    roaring_buffer_t *r2;
    uint64 card1;
    bool ret;

    r1 = roaring_buffer_create(VARDATA(serializedbytes1),
                               VARSIZE(serializedbytes1));
    if (!r1)
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));

    r2 = roaring_buffer_create(VARDATA(serializedbytes2),
                               VARSIZE(serializedbytes2));
    if (!r2) {
        roaring_buffer_free(r1);
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));
    }

    ret = roaring_buffer_and_cardinality(r1, r2, &card1);
    roaring_buffer_free(r1);
    roaring_buffer_free(r2);
    if(!ret)
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));

    PG_RETURN_INT64(card1);
}


//bitmap_andnot
PG_FUNCTION_INFO_V1(rb_andnot);
Datum rb_andnot(PG_FUNCTION_ARGS);

Datum
rb_andnot(PG_FUNCTION_ARGS) {
    bytea *serializedbytes1 = PG_GETARG_BYTEA_P(0);
    bytea *serializedbytes2 = PG_GETARG_BYTEA_P(1);
    roaring_buffer_t *r1;
    roaring_buffer_t *r2;
    roaring_bitmap_t *r;
    size_t expectedsize;
    bytea *serializedbytes;

    r1 = roaring_buffer_create(VARDATA(serializedbytes1),
                               VARSIZE(serializedbytes1));
    if (!r1)
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));

    r2 = roaring_buffer_create(VARDATA(serializedbytes2),
                               VARSIZE(serializedbytes2));
    if (!r2) {
        roaring_buffer_free(r1);
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));
    }

    r = roaring_buffer_andnot(r1, r2);
    roaring_buffer_free(r1);
    roaring_buffer_free(r2);
    if (!r) {
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));
    }

    expectedsize = roaring_bitmap_portable_size_in_bytes(r);
    serializedbytes = (bytea *) palloc(VARHDRSZ + expectedsize);
    roaring_bitmap_portable_serialize(r, VARDATA(serializedbytes));
    roaring_bitmap_free(r);

    SET_VARSIZE(serializedbytes, VARHDRSZ + expectedsize);
    PG_RETURN_BYTEA_P(serializedbytes);
}


//bitmap_andnot_cardinality
PG_FUNCTION_INFO_V1(rb_andnot_cardinality);
Datum rb_andnot_cardinality(PG_FUNCTION_ARGS);

Datum
rb_andnot_cardinality(PG_FUNCTION_ARGS) {
    bytea *serializedbytes1 = PG_GETARG_BYTEA_P(0);
    bytea *serializedbytes2 = PG_GETARG_BYTEA_P(1);
    roaring_buffer_t *r1;
    roaring_buffer_t *r2;
    uint64 card1;
    bool ret;

    r1 = roaring_buffer_create(VARDATA(serializedbytes1),
                               VARSIZE(serializedbytes1));
    if (!r1)
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));

    r2 = roaring_buffer_create(VARDATA(serializedbytes2),
                               VARSIZE(serializedbytes2));
    if (!r2) {
        roaring_buffer_free(r1);
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));
    }

    ret = roaring_buffer_andnot_cardinality(r1, r2, &card1);
    roaring_buffer_free(r1);
    roaring_buffer_free(r2);
    if(!ret)
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));

    PG_RETURN_INT64(card1);
}


//bitmap_xor
PG_FUNCTION_INFO_V1(rb_xor);
Datum rb_xor(PG_FUNCTION_ARGS);

Datum
rb_xor(PG_FUNCTION_ARGS) {
    bytea *serializedbytes1 = PG_GETARG_BYTEA_P(0);
    bytea *serializedbytes2 = PG_GETARG_BYTEA_P(1);
    roaring_bitmap_t *r1;
    roaring_bitmap_t *r2;
    size_t expectedsize;
    bytea *serializedbytes;

    r1 = roaring_bitmap_portable_deserialize(VARDATA(serializedbytes1));
    if (!r1)
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));

    r2 = roaring_bitmap_portable_deserialize(VARDATA(serializedbytes2));
    if (!r2) {
        roaring_bitmap_free(r1);
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));
    }

    roaring_bitmap_xor_inplace(r1, r2);
    roaring_bitmap_free(r2);
    expectedsize = roaring_bitmap_portable_size_in_bytes(r1);
    serializedbytes = (bytea *) palloc(VARHDRSZ + expectedsize);
    roaring_bitmap_portable_serialize(r1, VARDATA(serializedbytes));
    roaring_bitmap_free(r1);

    SET_VARSIZE(serializedbytes, VARHDRSZ + expectedsize);
    PG_RETURN_BYTEA_P(serializedbytes);
}


//bitmap_xor_cardinality
PG_FUNCTION_INFO_V1(rb_xor_cardinality);
Datum rb_xor_cardinality(PG_FUNCTION_ARGS);

Datum
rb_xor_cardinality(PG_FUNCTION_ARGS) {
    bytea *serializedbytes1 = PG_GETARG_BYTEA_P(0);
    bytea *serializedbytes2 = PG_GETARG_BYTEA_P(1);
    roaring_buffer_t *r1;
    roaring_buffer_t *r2;
    uint64 card1;
    bool ret;

    r1 = roaring_buffer_create(VARDATA(serializedbytes1),
                               VARSIZE(serializedbytes1));
    if (!r1)
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));

    r2 = roaring_buffer_create(VARDATA(serializedbytes2),
                               VARSIZE(serializedbytes2));
    if (!r2) {
        roaring_buffer_free(r1);
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));
    }

    ret = roaring_buffer_xor_cardinality(r1, r2, &card1);
    roaring_buffer_free(r1);
    roaring_buffer_free(r2);
    if(!ret)
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));

    PG_RETURN_INT64(card1);
}


//bitmap cardinality
PG_FUNCTION_INFO_V1(rb_cardinality);
Datum rb_cardinality(PG_FUNCTION_ARGS);

Datum
rb_cardinality(PG_FUNCTION_ARGS) {
    bytea *data = PG_GETARG_BYTEA_P(0);
    roaring_buffer_t *r1;
    uint64 card;

    r1 = roaring_buffer_create(VARDATA(data),
                               VARSIZE(data));
    if (!r1)
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));

    card = roaring_buffer_get_cardinality(r1);
    roaring_buffer_free(r1);

    PG_RETURN_INT64(card);
}


//bitmap is empty
PG_FUNCTION_INFO_V1(rb_is_empty);
Datum rb_is_empty(PG_FUNCTION_ARGS);

Datum
rb_is_empty(PG_FUNCTION_ARGS) {
    bytea *data = PG_GETARG_BYTEA_P(0);
    roaring_buffer_t *r1;
    bool isempty;

    r1 = roaring_buffer_create(VARDATA(data),
                               VARSIZE(data));
    if (!r1)
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));

    isempty = roaring_buffer_is_empty(r1);
    roaring_buffer_free(r1);

    PG_RETURN_INT64(isempty);
}

//bitmap contains one value
PG_FUNCTION_INFO_V1(rb_exsit);
Datum rb_exsit(PG_FUNCTION_ARGS);

Datum
rb_exsit(PG_FUNCTION_ARGS) {
    bytea *data = PG_GETARG_BYTEA_P(0);
    uint32 value = PG_GETARG_UINT32(1);
    roaring_buffer_t *r1;
    bool isexsit;
    bool ret;

    r1 = roaring_buffer_create(VARDATA(data),
                               VARSIZE(data));
    if (!r1)
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));

    ret = roaring_buffer_contains(r1, value, &isexsit);
    roaring_buffer_free(r1);
    if(!ret)
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));

    PG_RETURN_BOOL(isexsit);
}

//bitmap equals
PG_FUNCTION_INFO_V1(rb_equals);
Datum rb_equals(PG_FUNCTION_ARGS);

Datum
rb_equals(PG_FUNCTION_ARGS) {
    bytea *serializedbytes1 = PG_GETARG_BYTEA_P(0);
    bytea *serializedbytes2 = PG_GETARG_BYTEA_P(1);
    roaring_buffer_t *r1;
    roaring_buffer_t *r2;
    bool isequal;
    bool ret;

    r1 = roaring_buffer_create(VARDATA(serializedbytes1),
                               VARSIZE(serializedbytes1));
    if (!r1)
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));

    r2 = roaring_buffer_create(VARDATA(serializedbytes2),
                               VARSIZE(serializedbytes2));
    if (!r2) {
        roaring_buffer_free(r1);
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));
    }

    ret = roaring_buffer_equals(r1, r2, &isequal);
    roaring_buffer_free(r1);
    roaring_buffer_free(r2);
    if(!ret)
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));

    PG_RETURN_BOOL(isequal);
}

//bitmap not equals
PG_FUNCTION_INFO_V1(rb_not_equals);
Datum rb_not_equals(PG_FUNCTION_ARGS);

Datum
rb_not_equals(PG_FUNCTION_ARGS) {
    bytea *serializedbytes1 = PG_GETARG_BYTEA_P(0);
    bytea *serializedbytes2 = PG_GETARG_BYTEA_P(1);
    roaring_buffer_t *r1;
    roaring_buffer_t *r2;
    bool isequal;
    bool ret;

    r1 = roaring_buffer_create(VARDATA(serializedbytes1),
                               VARSIZE(serializedbytes1));
    if (!r1)
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));

    r2 = roaring_buffer_create(VARDATA(serializedbytes2),
                               VARSIZE(serializedbytes2));
    if (!r2) {
        roaring_buffer_free(r1);
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));
    }

    ret = roaring_buffer_equals(r1, r2, &isequal);
    roaring_buffer_free(r1);
    roaring_buffer_free(r2);
    if(!ret)
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));

    PG_RETURN_BOOL(!isequal);
}

//bitmap intersect
PG_FUNCTION_INFO_V1(rb_intersect);
Datum rb_intersect(PG_FUNCTION_ARGS);

Datum
rb_intersect(PG_FUNCTION_ARGS) {
    bytea *serializedbytes1 = PG_GETARG_BYTEA_P(0);
    bytea *serializedbytes2 = PG_GETARG_BYTEA_P(1);
    roaring_buffer_t *r1;
    roaring_buffer_t *r2;
    bool isintersect;
    bool ret;

    r1 = roaring_buffer_create(VARDATA(serializedbytes1),
                               VARSIZE(serializedbytes1));
    if (!r1)
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));

    r2 = roaring_buffer_create(VARDATA(serializedbytes2),
                               VARSIZE(serializedbytes2));
    if (!r2) {
        roaring_buffer_free(r1);
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));
    }

    ret = roaring_buffer_intersect(r1, r2, &isintersect);
    roaring_buffer_free(r1);
    roaring_buffer_free(r2);
    if(!ret)
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));

    PG_RETURN_BOOL(isintersect);
}

//bitmap contains
PG_FUNCTION_INFO_V1(rb_contains);
Datum rb_contains(PG_FUNCTION_ARGS);

Datum
rb_contains(PG_FUNCTION_ARGS) {
    bytea *serializedbytes1 = PG_GETARG_BYTEA_P(0);
    bytea *serializedbytes2 = PG_GETARG_BYTEA_P(1);
    roaring_buffer_t *r1;
    roaring_buffer_t *r2;
    bool iscontain;
    bool ret;

    r1 = roaring_buffer_create(VARDATA(serializedbytes1),
                               VARSIZE(serializedbytes1));
    if (!r1)
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));

    r2 = roaring_buffer_create(VARDATA(serializedbytes2),
                               VARSIZE(serializedbytes2));
    if (!r2) {
        roaring_buffer_free(r1);
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));
    }

    ret = roaring_buffer_is_subset(r2, r1, &iscontain);
    roaring_buffer_free(r1);
    roaring_buffer_free(r2);
    if(!ret)
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));

    PG_RETURN_BOOL(iscontain);
}

//bitmap contained
PG_FUNCTION_INFO_V1(rb_containedby);
Datum rb_containedby(PG_FUNCTION_ARGS);

Datum
rb_containedby(PG_FUNCTION_ARGS) {
    bytea *serializedbytes1 = PG_GETARG_BYTEA_P(0);
    bytea *serializedbytes2 = PG_GETARG_BYTEA_P(1);
    roaring_buffer_t *r1;
    roaring_buffer_t *r2;
    bool iscontained;
    bool ret;

    r1 = roaring_buffer_create(VARDATA(serializedbytes1),
                               VARSIZE(serializedbytes1));
    if (!r1)
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));

    r2 = roaring_buffer_create(VARDATA(serializedbytes2),
                               VARSIZE(serializedbytes2));
    if (!r2) {
        roaring_buffer_free(r1);
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));
    }

    ret = roaring_buffer_is_subset(r1, r2, &iscontained);
    roaring_buffer_free(r1);
    roaring_buffer_free(r2);
    if(!ret)
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));

    PG_RETURN_BOOL(iscontained);
}

//bitmap jaccard distance
PG_FUNCTION_INFO_V1(rb_jaccard_dist);
Datum rb_jaccard_dist(PG_FUNCTION_ARGS);

Datum
rb_jaccard_dist(PG_FUNCTION_ARGS) {
    bytea *serializedbytes1 = PG_GETARG_BYTEA_P(0);
    bytea *serializedbytes2 = PG_GETARG_BYTEA_P(1);
    roaring_buffer_t *r1;
    roaring_buffer_t *r2;
    double jaccard_dist;
    bool ret;

    r1 = roaring_buffer_create(VARDATA(serializedbytes1),
                               VARSIZE(serializedbytes1));
    if (!r1)
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));

    r2 = roaring_buffer_create(VARDATA(serializedbytes2),
                               VARSIZE(serializedbytes2));
    if (!r2) {
        roaring_buffer_free(r1);
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));
    }

    ret = roaring_buffer_jaccard_index(r1, r2, &jaccard_dist);
    roaring_buffer_free(r1);
    roaring_buffer_free(r2);
    if(!ret)
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));

    PG_RETURN_FLOAT8(jaccard_dist);
}

//bitmap add
PG_FUNCTION_INFO_V1(rb_add);
Datum rb_add(PG_FUNCTION_ARGS);

Datum
rb_add(PG_FUNCTION_ARGS) {
    bytea *serializedbytes1 = PG_GETARG_BYTEA_P(0);
    uint32 value = PG_GETARG_UINT32(1);
    size_t expectedsize;
    bytea *serializedbytes;

    roaring_bitmap_t *r1 = roaring_bitmap_portable_deserialize(VARDATA(serializedbytes1));
    if (!r1)
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));

    roaring_bitmap_add(r1, value);

    expectedsize = roaring_bitmap_portable_size_in_bytes(r1);
    serializedbytes = (bytea *) palloc(VARHDRSZ + expectedsize);
    roaring_bitmap_portable_serialize(r1, VARDATA(serializedbytes));
    roaring_bitmap_free(r1);

    SET_VARSIZE(serializedbytes, VARHDRSZ + expectedsize);
    PG_RETURN_BYTEA_P(serializedbytes);
}

//bitmap remove
PG_FUNCTION_INFO_V1(rb_remove);
Datum rb_remove(PG_FUNCTION_ARGS);

Datum
rb_remove(PG_FUNCTION_ARGS) {
    bytea *serializedbytes1 = PG_GETARG_BYTEA_P(0);
    uint32 value = PG_GETARG_UINT32(1);
    size_t expectedsize;
    bytea *serializedbytes;

    roaring_bitmap_t *r1 = roaring_bitmap_portable_deserialize(VARDATA(serializedbytes1));
    if (!r1)
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));

    roaring_bitmap_remove(r1, value);

    expectedsize = roaring_bitmap_portable_size_in_bytes(r1);
    serializedbytes = (bytea *) palloc(VARHDRSZ + expectedsize);
    roaring_bitmap_portable_serialize(r1, VARDATA(serializedbytes));
    roaring_bitmap_free(r1);

    SET_VARSIZE(serializedbytes, VARHDRSZ + expectedsize);
    PG_RETURN_BYTEA_P(serializedbytes);
}

//bitmap minimum
PG_FUNCTION_INFO_V1(rb_min);
Datum rb_min(PG_FUNCTION_ARGS);

Datum
rb_min(PG_FUNCTION_ARGS) {
    bytea *data = PG_GETARG_BYTEA_P(0);
    roaring_buffer_t *r1;
    uint32 min;
    bool ret;

    r1 = roaring_buffer_create(VARDATA(data),
                               VARSIZE(data));
    if (!r1)
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));

    if(roaring_buffer_is_empty(r1))
    {
        roaring_buffer_free(r1);
        PG_RETURN_NULL();
    }

    ret = roaring_buffer_minimum(r1, &min);
    roaring_buffer_free(r1);
    if(!ret)
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));

    PG_RETURN_UINT32(min);
}


//bitmap maximum
PG_FUNCTION_INFO_V1(rb_max);
Datum rb_max(PG_FUNCTION_ARGS);

Datum
rb_max(PG_FUNCTION_ARGS) {
    bytea *data = PG_GETARG_BYTEA_P(0);
    roaring_buffer_t *r1;
    uint32 max;
    bool ret;

    r1 = roaring_buffer_create(VARDATA(data),
                               VARSIZE(data));
    if (!r1)
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));

    if(roaring_buffer_is_empty(r1))
    {
        roaring_buffer_free(r1);
        PG_RETURN_NULL();
    }

    ret = roaring_buffer_maximum(r1, &max);
    roaring_buffer_free(r1);
    if(!ret)
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));

    PG_RETURN_UINT32(max);
}

//bitmap rank
PG_FUNCTION_INFO_V1(rb_rank);
Datum rb_rank(PG_FUNCTION_ARGS);

Datum
rb_rank(PG_FUNCTION_ARGS) {
    bytea *data = PG_GETARG_BYTEA_P(0);
    uint32 value = PG_GETARG_UINT32(1);
    roaring_buffer_t *r1;
    uint64 rank;
    bool ret;

    r1 = roaring_buffer_create(VARDATA(data),
                               VARSIZE(data));
    if (!r1)
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));

    ret = roaring_buffer_rank(r1, value, &rank);
    roaring_buffer_free(r1);
    if(!ret)
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));

    PG_RETURN_INT64((int64)rank);
}

//bitmap index
PG_FUNCTION_INFO_V1(rb_index);
Datum rb_index(PG_FUNCTION_ARGS);

Datum
rb_index(PG_FUNCTION_ARGS) {
    bytea *data = PG_GETARG_BYTEA_P(0);
    uint32 value = PG_GETARG_UINT32(1);
    roaring_buffer_t *r1;
    uint64 rank;
    int64 result;
    bool ret,isexsit;

    r1 = roaring_buffer_create(VARDATA(data),
                               VARSIZE(data));
    if (!r1)
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));

    ret = roaring_buffer_contains(r1, value, &isexsit);
    if(!ret)
    {
        roaring_buffer_free(r1);
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));
    }

    result = -1;
    if(isexsit)
    {
        ret = roaring_buffer_rank(r1, value, &rank);
        roaring_buffer_free(r1);
        if(!ret)
            ereport(ERROR,
                    (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                    errmsg("bitmap format is error")));

        result = (int64)rank - 1;
    }

    PG_RETURN_INT64(result);
}

//bitmap fill
PG_FUNCTION_INFO_V1(rb_fill);
Datum rb_fill(PG_FUNCTION_ARGS);

Datum
rb_fill(PG_FUNCTION_ARGS) {
    bytea *serializedbytes1 = PG_GETARG_BYTEA_P(0);
    int64 rangestart = PG_GETARG_INT64(1);
    int64 rangeend = PG_GETARG_INT64(2);
    roaring_bitmap_t *r1;
    roaring_bitmap_t *r2;
    size_t expectedsize;
    bytea *serializedbytes;

    if (rangestart < 0)
        rangestart = 0;
    if (rangeend < 0)
        rangeend = 0;
    if (rangeend > MAX_BITMAP_RANGE_END) {
        rangeend = MAX_BITMAP_RANGE_END;
    }

    r1 = roaring_bitmap_portable_deserialize(VARDATA(serializedbytes1));
    if (!r1)
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));

    if (rangestart < rangeend) {
        r2 = roaring_bitmap_from_range(rangestart, rangeend, 1);
        if (!r2) {
            roaring_bitmap_free(r1);
            ereport(ERROR,
                    (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                     errmsg("failed to create bitmap")));
        }
        roaring_bitmap_or_inplace(r1, r2);
        roaring_bitmap_free(r2);
    }

    expectedsize = roaring_bitmap_portable_size_in_bytes(r1);
    serializedbytes = (bytea *) palloc(VARHDRSZ + expectedsize);
    roaring_bitmap_portable_serialize(r1, VARDATA(serializedbytes));
    roaring_bitmap_free(r1);

    SET_VARSIZE(serializedbytes, VARHDRSZ + expectedsize);
    PG_RETURN_BYTEA_P(serializedbytes);
}

//bitmap clear
PG_FUNCTION_INFO_V1(rb_clear);
Datum rb_clear(PG_FUNCTION_ARGS);

Datum
rb_clear(PG_FUNCTION_ARGS) {
    bytea *serializedbytes1 = PG_GETARG_BYTEA_P(0);
    int64 rangestart = PG_GETARG_INT64(1);
    int64 rangeend = PG_GETARG_INT64(2);
    roaring_bitmap_t *r1;
    roaring_bitmap_t *r2;
    size_t expectedsize;
    bytea *serializedbytes;

    if (rangestart < 0)
        rangestart = 0;
    if (rangeend < 0)
        rangeend = 0;
    if (rangeend > MAX_BITMAP_RANGE_END) {
        rangeend = MAX_BITMAP_RANGE_END;
    }

    r1 = roaring_bitmap_portable_deserialize(VARDATA(serializedbytes1));
    if (!r1)
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));

    if (rangestart < rangeend) {
        r2 = roaring_bitmap_from_range(rangestart, rangeend, 1);
        if (!r2) {
            roaring_bitmap_free(r1);
            ereport(ERROR,
                    (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                     errmsg("failed to create bitmap")));
        }

        roaring_bitmap_andnot_inplace(r1, r2);
        roaring_bitmap_free(r2);
    }

    expectedsize = roaring_bitmap_portable_size_in_bytes(r1);
    serializedbytes = (bytea *) palloc(VARHDRSZ + expectedsize);
    roaring_bitmap_portable_serialize(r1, VARDATA(serializedbytes));
    roaring_bitmap_free(r1);

    SET_VARSIZE(serializedbytes, VARHDRSZ + expectedsize);
    PG_RETURN_BYTEA_P(serializedbytes);
}

//bitmap flip
PG_FUNCTION_INFO_V1(rb_flip);
Datum rb_flip(PG_FUNCTION_ARGS);

Datum
rb_flip(PG_FUNCTION_ARGS) {
    bytea *serializedbytes1 = PG_GETARG_BYTEA_P(0);
    int64 rangestart = PG_GETARG_INT64(1);
    int64 rangeend = PG_GETARG_INT64(2);
    roaring_bitmap_t *r1;
    size_t expectedsize;
    bytea *serializedbytes;

    if (rangestart < 0)
        rangestart = 0;
    if (rangeend < 0)
        rangeend = 0;
    if (rangeend > MAX_BITMAP_RANGE_END) {
        rangeend = MAX_BITMAP_RANGE_END;
    }

    r1 = roaring_bitmap_portable_deserialize(VARDATA(serializedbytes1));
    if (!r1)
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));

    if (rangestart < rangeend) {
        roaring_bitmap_flip_inplace(r1, rangestart, rangeend);
    }

    expectedsize = roaring_bitmap_portable_size_in_bytes(r1);
    serializedbytes = (bytea *) palloc(VARHDRSZ + expectedsize);
    roaring_bitmap_portable_serialize(r1, VARDATA(serializedbytes));
    roaring_bitmap_free(r1);

    SET_VARSIZE(serializedbytes, VARHDRSZ + expectedsize);
    PG_RETURN_BYTEA_P(serializedbytes);
}

//bitmap shiftright
PG_FUNCTION_INFO_V1(rb_shiftright);
Datum rb_shiftright(PG_FUNCTION_ARGS);

Datum
rb_shiftright(PG_FUNCTION_ARGS) {
    bytea *serializedbytes1 = PG_GETARG_BYTEA_P(0);
    int64 distance = PG_GETARG_INT64(1);
    uint64 value;
    roaring_bitmap_t *r1;
    roaring_bitmap_t *r2;
    roaring_uint32_iterator_t iterator;
    size_t expectedsize;
    bytea *serializedbytes;

    r1 = roaring_bitmap_portable_deserialize(VARDATA(serializedbytes1));
    if (!r1)
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));

    if(distance != 0)
    {
        r2 = roaring_bitmap_create();
        if (!r2) {
            roaring_bitmap_free(r1);
            ereport(ERROR,
                    (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                     errmsg("failed to create bitmap")));
        }

        roaring_iterator_init(r1, &iterator);
        if(distance > 0){
            while(iterator.has_value) {
                value = iterator.current_value + distance;
                if(value >= MAX_BITMAP_RANGE_END)
                    break;
                roaring_bitmap_add(r2, (uint32)value);
                roaring_uint32_iterator_advance(&iterator);
            }
        }else{
            roaring_uint32_iterator_move_equalorlarger(&iterator, -distance);
            while(iterator.has_value) {
                value = iterator.current_value + distance;
                if(value >= MAX_BITMAP_RANGE_END)
                    break;
                roaring_bitmap_add(r2, (uint32)value);
                roaring_uint32_iterator_advance(&iterator);
            }
        }
        roaring_bitmap_free(r1);
        r1 = r2;
    }

    expectedsize = roaring_bitmap_portable_size_in_bytes(r1);
    serializedbytes = (bytea *) palloc(VARHDRSZ + expectedsize);
    roaring_bitmap_portable_serialize(r1, VARDATA(serializedbytes));
    roaring_bitmap_free(r1);

    SET_VARSIZE(serializedbytes, VARHDRSZ + expectedsize);
    PG_RETURN_BYTEA_P(serializedbytes);
}

//bitmap range
PG_FUNCTION_INFO_V1(rb_range);
Datum rb_range(PG_FUNCTION_ARGS);

Datum
rb_range(PG_FUNCTION_ARGS) {
    bytea *serializedbytes1 = PG_GETARG_BYTEA_P(0);
    int64 rangestart = PG_GETARG_INT64(1);
    int64 rangeend = PG_GETARG_INT64(2);
    roaring_bitmap_t *r1;
    roaring_bitmap_t *r2;
    roaring_uint32_iterator_t iterator;
    size_t expectedsize;
    bytea *serializedbytes;

    if (rangestart < 0)
        rangestart = 0;
    if (rangeend < 0)
        rangeend = 0;
    if (rangeend > MAX_BITMAP_RANGE_END) {
        rangeend = MAX_BITMAP_RANGE_END;
    }

    r1 = roaring_bitmap_portable_deserialize(VARDATA(serializedbytes1));
    if (!r1)
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));
    
    r2 = roaring_bitmap_create();
    if (!r2) {
        roaring_bitmap_free(r1);
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("failed to create bitmap")));
    }

    roaring_iterator_init(r1, &iterator);
    roaring_uint32_iterator_move_equalorlarger(&iterator, rangestart);
    while(iterator.has_value) {
        if(iterator.current_value >= rangeend)
            break;
        roaring_bitmap_add(r2, iterator.current_value);
        roaring_uint32_iterator_advance(&iterator);
    }

    expectedsize = roaring_bitmap_portable_size_in_bytes(r2);
    serializedbytes = (bytea *) palloc(VARHDRSZ + expectedsize);
    roaring_bitmap_portable_serialize(r2, VARDATA(serializedbytes));
    roaring_bitmap_free(r1);
    roaring_bitmap_free(r2);

    SET_VARSIZE(serializedbytes, VARHDRSZ + expectedsize);
    PG_RETURN_BYTEA_P(serializedbytes);
}

//bitmap range_cardinality
PG_FUNCTION_INFO_V1(rb_range_cardinality);
Datum rb_range_cardinality(PG_FUNCTION_ARGS);

Datum
rb_range_cardinality(PG_FUNCTION_ARGS) {
    bytea *serializedbytes1 = PG_GETARG_BYTEA_P(0);
    int64 rangestart = PG_GETARG_INT64(1);
    int64 rangeend = PG_GETARG_INT64(2);
    roaring_bitmap_t *r1;
    roaring_uint32_iterator_t iterator;
    uint64 card1;

    if (rangestart < 0)
        rangestart = 0;
    if (rangeend < 0)
        rangeend = 0;
    if (rangeend > MAX_BITMAP_RANGE_END) {
        rangeend = MAX_BITMAP_RANGE_END;
    }

    r1 = roaring_bitmap_portable_deserialize(VARDATA(serializedbytes1));
    if (!r1)
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));

    card1 = 0;
    roaring_iterator_init(r1, &iterator);
    roaring_uint32_iterator_move_equalorlarger(&iterator, rangestart);
    while(iterator.has_value) {
        if(iterator.current_value >= rangeend)
            break;
        card1++;
        roaring_uint32_iterator_advance(&iterator);
    }

    roaring_bitmap_free(r1);
    PG_RETURN_INT64(card1);
}

//bitmap range
PG_FUNCTION_INFO_V1(rb_select);
Datum rb_select(PG_FUNCTION_ARGS);

Datum
rb_select(PG_FUNCTION_ARGS) {
    bytea *serializedbytes1 = PG_GETARG_BYTEA_P(0);
    int64 limit = PG_GETARG_INT64(1);
    int64 offset = PG_GETARG_INT64(2);
    bool reverse = PG_GETARG_BOOL(3);
    int64 rangestart = PG_GETARG_INT64(4);
    int64 rangeend = PG_GETARG_INT64(5);
    int64 count = 0;
    int64 total_count = 0;
    roaring_bitmap_t *r1;
    roaring_bitmap_t *r2;
    roaring_uint32_iterator_t iterator;
    size_t expectedsize;
    bytea *serializedbytes;

    if (rangestart < 0)
        rangestart = 0;
    if (rangeend < 0)
        rangeend = 0;
    if (rangeend > MAX_BITMAP_RANGE_END) {
        rangeend = MAX_BITMAP_RANGE_END;
    }

    r1 = roaring_bitmap_portable_deserialize(VARDATA(serializedbytes1));
    if (!r1)
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));

    r2 = roaring_bitmap_create();
    if (!r2) {
        roaring_bitmap_free(r1);
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("failed to create bitmap")));
    }

    if (limit > 0) {
        roaring_iterator_init(r1, &iterator);
        roaring_uint32_iterator_move_equalorlarger(&iterator, rangestart);
        if (!reverse) {
            while (iterator.has_value) {
                if (iterator.current_value >= rangeend
                        || count - offset >= limit)
                    break;
                if (count >= offset) {
                    roaring_bitmap_add(r2, iterator.current_value);
                }
                roaring_uint32_iterator_advance(&iterator);
                count++;
            }
        } else {
            while (iterator.has_value) {
                if (iterator.current_value >= rangeend)
                    break;
                roaring_uint32_iterator_advance(&iterator);
                total_count++;
            }

            if (total_count > offset) {
                /* calulate new offset for reverse */
                offset = total_count - offset - limit;
                if(offset < 0)
                    offset = 0;
                roaring_iterator_init(r1, &iterator);
                roaring_uint32_iterator_move_equalorlarger(&iterator,rangestart);
                count = 0;
                while (iterator.has_value) {
                    if (iterator.current_value >= rangeend
                            || count - offset >= limit)
                        break;
                    if (count >= offset) {
                        roaring_bitmap_add(r2, iterator.current_value);
                    }
                    roaring_uint32_iterator_advance(&iterator);
                    count++;
                }
            }
        }
    }

    expectedsize = roaring_bitmap_portable_size_in_bytes(r2);
    serializedbytes = (bytea *) palloc(VARHDRSZ + expectedsize);
    roaring_bitmap_portable_serialize(r2, VARDATA(serializedbytes));
    roaring_bitmap_free(r1);
    roaring_bitmap_free(r2);

    SET_VARSIZE(serializedbytes, VARHDRSZ + expectedsize);
    PG_RETURN_BYTEA_P(serializedbytes);
}

//bitmap build
PG_FUNCTION_INFO_V1(rb_build);
Datum rb_build(PG_FUNCTION_ARGS);

Datum
rb_build(PG_FUNCTION_ARGS) {
    ArrayType *a = (ArrayType *) PG_GETARG_ARRAYTYPE_P(0);
    int na, n;
    int *da;
    roaring_bitmap_t *r1;
    size_t expectedsize;
    bytea *serializedbytes;

    CHECKARRVALID(a);

    na = ARRNELEMS(a);
    da = ARRPTR(a);

    r1 = roaring_bitmap_create();

    for (n = 0; n < na; n++) {
        roaring_bitmap_add(r1, da[n]);
    }

    expectedsize = roaring_bitmap_portable_size_in_bytes(r1);

    serializedbytes = (bytea *) palloc(VARHDRSZ + expectedsize);
    roaring_bitmap_portable_serialize(r1, VARDATA(serializedbytes));
    roaring_bitmap_free(r1);

    SET_VARSIZE(serializedbytes, VARHDRSZ + expectedsize);
    PG_RETURN_BYTEA_P(serializedbytes);
}

//bitmap to int[]
PG_FUNCTION_INFO_V1(rb_to_array);
Datum rb_to_array(PG_FUNCTION_ARGS);

Datum
rb_to_array(PG_FUNCTION_ARGS)
{
    bytea *serializedbytes = PG_GETARG_BYTEA_P(0);
    roaring_bitmap_t *r1;
    roaring_uint32_iterator_t *iterator;
    ArrayType *result;
    Datum *out_datums;
    uint64_t card1;
    uint32_t counter = 0;

    r1 = roaring_bitmap_portable_deserialize(VARDATA(serializedbytes));
    if (!r1)
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));

    card1 = roaring_bitmap_get_cardinality(r1);

    if (card1 == 0)
    {
        result = construct_empty_array(INT4OID);
    }
    else
    {
        out_datums = (Datum *)palloc(sizeof(Datum) * card1);

        iterator = roaring_iterator_create(r1);
        while (iterator->has_value)
        {
            out_datums[counter] = Int32GetDatum(iterator->current_value);
            counter++;
            roaring_uint32_iterator_advance(iterator);
        }
        roaring_uint32_iterator_free(iterator);

        result = construct_array(out_datums, card1, INT4OID, sizeof(int32), true, 'i');
    }

    roaring_bitmap_free(r1);
    PG_RETURN_POINTER(result);
}

//bitmap list
PG_FUNCTION_INFO_V1(rb_iterate);
Datum rb_iterate(PG_FUNCTION_ARGS);

Datum
rb_iterate(PG_FUNCTION_ARGS) {
    FuncCallContext *funcctx;
    MemoryContext oldcontext;
    roaring_uint32_iterator_t *fctx;
    bytea *data;
    roaring_bitmap_t *r1;

    if (SRF_IS_FIRSTCALL()) {

        funcctx = SRF_FIRSTCALL_INIT();

        data = PG_GETARG_BYTEA_P(0);

        oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

        r1 = roaring_bitmap_portable_deserialize(VARDATA(data));
        if (!r1)
            ereport(ERROR,
                    (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                     errmsg("bitmap format is error")));

        fctx = roaring_iterator_create(r1);

        funcctx->user_fctx = fctx;

        MemoryContextSwitchTo(oldcontext);
    }

    funcctx = SRF_PERCALL_SETUP();

    fctx = funcctx->user_fctx;

    if (fctx->has_value) {
        Datum result;
        result = fctx->current_value;
        roaring_uint32_iterator_advance(fctx);
        SRF_RETURN_NEXT(funcctx, result);
    } else {
        roaring_uint32_iterator_free(fctx);
        SRF_RETURN_DONE(funcctx);
    }
}

//bitmap or trans
PG_FUNCTION_INFO_V1(rb_or_trans);
Datum rb_or_trans(PG_FUNCTION_ARGS);

Datum
rb_or_trans(PG_FUNCTION_ARGS) {
    MemoryContext aggctx;
    MemoryContext oldcontext;
    bytea *bb;
    roaring_bitmap_t *r1;
    roaring_bitmap_t *r2;

    // We must be called as a transition routine or we fail.
    if (!AggCheckCallContext(fcinfo, &aggctx))
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("rb_or_trans outside transition context")));

    if (PG_ARGISNULL(1)) {
        if (PG_ARGISNULL(0)) {
            PG_RETURN_NULL();
        }
        r1 = (roaring_bitmap_t *) PG_GETARG_POINTER(0);
    } else {
        bb = PG_GETARG_BYTEA_P(1);

        oldcontext = MemoryContextSwitchTo(aggctx);

        r2 = roaring_bitmap_portable_deserialize(VARDATA(bb));

        if (PG_ARGISNULL(0)) {
            r1 = r2;
        } else {
            r1 = (roaring_bitmap_t *) PG_GETARG_POINTER(0);
            roaring_bitmap_or_inplace(r1, r2);
            roaring_bitmap_free(r2);
        }

        MemoryContextSwitchTo(oldcontext);
    }

    PG_RETURN_POINTER(r1);
}

//bitmap or combine
PG_FUNCTION_INFO_V1(rb_or_combine);
Datum rb_or_combine(PG_FUNCTION_ARGS);

Datum
rb_or_combine(PG_FUNCTION_ARGS) {
    MemoryContext aggctx;
    MemoryContext oldcontext;
    roaring_bitmap_t *r1;
    roaring_bitmap_t *r2;

    // We must be called as a transition routine or we fail.
    if (!AggCheckCallContext(fcinfo, &aggctx))
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("rb_or_combine outside transition context")));

    if (PG_ARGISNULL(1)) {
        if (PG_ARGISNULL(0)) {
            PG_RETURN_NULL();
        }
        r1 = (roaring_bitmap_t *) PG_GETARG_POINTER(0);
    } else {
        r2 = (roaring_bitmap_t *) PG_GETARG_POINTER(1);

        oldcontext = MemoryContextSwitchTo(aggctx);

        if (PG_ARGISNULL(0)) {
            r1 = roaring_bitmap_copy(r2);
        } else {
            r1 = (roaring_bitmap_t *) PG_GETARG_POINTER(0);
            roaring_bitmap_or_inplace(r1, r2);
        }

        MemoryContextSwitchTo(oldcontext);
    }

    PG_RETURN_POINTER(r1);
}

//bitmap and trans
PG_FUNCTION_INFO_V1(rb_and_trans);
Datum rb_and_trans(PG_FUNCTION_ARGS);

Datum
rb_and_trans(PG_FUNCTION_ARGS) {
    MemoryContext aggctx;
    MemoryContext oldcontext;
    bytea *bb;
    roaring_bitmap_t *r1;
    roaring_bitmap_t *r2;

    // We must be called as a transition routine or we fail.
    if (!AggCheckCallContext(fcinfo, &aggctx))
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("rb_and_trans outside transition context")));

    if (PG_ARGISNULL(1)) {
        if (PG_ARGISNULL(0)) {
            PG_RETURN_NULL();
        }
        r1 = (roaring_bitmap_t *) PG_GETARG_POINTER(0);
    } else {
        if (PG_ARGISNULL(0) ) {
            /* postgres will crash when use PG_GETARG_BYTEA_PP here */
            bb = PG_GETARG_BYTEA_P(1);

            oldcontext = MemoryContextSwitchTo(aggctx);
            r2 = roaring_bitmap_portable_deserialize(VARDATA(bb));
            MemoryContextSwitchTo(oldcontext);
            r1 = r2;
        } else {
            r1 = (roaring_bitmap_t *) PG_GETARG_POINTER(0);
            if (!roaring_bitmap_is_empty(r1)) {
                bb = PG_GETARG_BYTEA_P(1);
                r2 = roaring_bitmap_portable_deserialize(VARDATA(bb));

                oldcontext = MemoryContextSwitchTo(aggctx);
                roaring_bitmap_and_inplace(r1, r2);
                MemoryContextSwitchTo(oldcontext);

                roaring_bitmap_free(r2);
            }
        }
    }

    PG_RETURN_POINTER(r1);
}

//bitmap and combine
PG_FUNCTION_INFO_V1(rb_and_combine);
Datum rb_and_combine(PG_FUNCTION_ARGS);

Datum
rb_and_combine(PG_FUNCTION_ARGS) {
    MemoryContext aggctx;
    MemoryContext oldcontext;
    roaring_bitmap_t *r1;
    roaring_bitmap_t *r2;

    // We must be called as a transition routine or we fail.
    if (!AggCheckCallContext(fcinfo, &aggctx))
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("rb_and_combine outside transition context")));

    if (PG_ARGISNULL(1)) {
        if (PG_ARGISNULL(0)) {
            PG_RETURN_NULL();
        }
        r1 = (roaring_bitmap_t *) PG_GETARG_POINTER(0);
    } else {
        r2 = (roaring_bitmap_t *) PG_GETARG_POINTER(1);

        oldcontext = MemoryContextSwitchTo(aggctx);

        if (PG_ARGISNULL(0)) {
            r1 = roaring_bitmap_copy(r2);
        } else {
            r1 = (roaring_bitmap_t *) PG_GETARG_POINTER(0);
            roaring_bitmap_and_inplace(r1, r2);
        }

        MemoryContextSwitchTo(oldcontext);
    }

    PG_RETURN_POINTER(r1);
}

//bitmap xor trans
PG_FUNCTION_INFO_V1(rb_xor_trans);
Datum rb_xor_trans(PG_FUNCTION_ARGS);

Datum
rb_xor_trans(PG_FUNCTION_ARGS) {
    MemoryContext aggctx;
    MemoryContext oldcontext;
    bytea *bb;
    roaring_bitmap_t *r1;
    roaring_bitmap_t *r2;

    // We must be called as a transition routine or we fail.
    if (!AggCheckCallContext(fcinfo, &aggctx))
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("rb_xor_trans outside transition context")));

    if (PG_ARGISNULL(1)) {
        if (PG_ARGISNULL(0)) {
            PG_RETURN_NULL();
        }
        r1 = (roaring_bitmap_t *) PG_GETARG_POINTER(0);
    } else {
        bb = PG_GETARG_BYTEA_P(1);

        oldcontext = MemoryContextSwitchTo(aggctx);

        r2 = roaring_bitmap_portable_deserialize(VARDATA(bb));

        if (PG_ARGISNULL(0)) {
            r1 = r2;
        } else {
            r1 = (roaring_bitmap_t *) PG_GETARG_POINTER(0);
            roaring_bitmap_xor_inplace(r1, r2);
            roaring_bitmap_free(r2);
        }

        MemoryContextSwitchTo(oldcontext);
    }

    PG_RETURN_POINTER(r1);
}

//bitmap xor combine
PG_FUNCTION_INFO_V1(rb_xor_combine);
Datum rb_xor_combine(PG_FUNCTION_ARGS);

Datum
rb_xor_combine(PG_FUNCTION_ARGS) {
    MemoryContext aggctx;
    MemoryContext oldcontext;
    roaring_bitmap_t *r1;
    roaring_bitmap_t *r2;

    // We must be called as a transition routine or we fail.
    if (!AggCheckCallContext(fcinfo, &aggctx))
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("rb_xor_combine outside transition context")));

    if (PG_ARGISNULL(1)) {
        if (PG_ARGISNULL(0)) {
            PG_RETURN_NULL();
        }
        r1 = (roaring_bitmap_t *) PG_GETARG_POINTER(0);
    } else {
        r2 = (roaring_bitmap_t *) PG_GETARG_POINTER(1);

        oldcontext = MemoryContextSwitchTo(aggctx);

        if (PG_ARGISNULL(0)) {
            r1 = roaring_bitmap_copy(r2);
        } else {
            r1 = (roaring_bitmap_t *) PG_GETARG_POINTER(0);
            roaring_bitmap_xor_inplace(r1, r2);
        }

        MemoryContextSwitchTo(oldcontext);
    }

    PG_RETURN_POINTER(r1);
}

//bitmap build trans
PG_FUNCTION_INFO_V1(rb_build_trans);
Datum rb_build_trans(PG_FUNCTION_ARGS);

Datum
rb_build_trans(PG_FUNCTION_ARGS) {
    MemoryContext aggctx;
    MemoryContext oldcontext;
    roaring_bitmap_t *r1;
    int i2;

    // We must be called as a transition routine or we fail.
    if (!AggCheckCallContext(fcinfo, &aggctx))
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("rb_build_trans outside transition context")));

    if (PG_ARGISNULL(1)) {
        if (PG_ARGISNULL(0)) {
            PG_RETURN_NULL();
        }
        r1 = (roaring_bitmap_t *) PG_GETARG_POINTER(0);
    } else {
        i2 = PG_GETARG_UINT32(1);

        oldcontext = MemoryContextSwitchTo(aggctx);

        if (PG_ARGISNULL(0)) {
            r1 = roaring_bitmap_create();
        } else {
            r1 = (roaring_bitmap_t *) PG_GETARG_POINTER(0);
        }
        roaring_bitmap_add(r1, i2);

        MemoryContextSwitchTo(oldcontext);
    }

    PG_RETURN_POINTER(r1);
}


//bitmap Serialize
PG_FUNCTION_INFO_V1(rb_serialize);
Datum rb_serialize(PG_FUNCTION_ARGS);

Datum
rb_serialize(PG_FUNCTION_ARGS) {
    MemoryContext aggctx;
    roaring_bitmap_t *r1;
    size_t expectedsize;
    bytea *serializedbytes;

    // We must be called as a transition routine or we fail.
    if (!AggCheckCallContext(fcinfo, &aggctx))
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("rb_serialize outside aggregate context")));

    // Is the first argument a NULL?
    if (PG_ARGISNULL(0)) {
        PG_RETURN_NULL();
    } else {
        r1 = (roaring_bitmap_t *) PG_GETARG_POINTER(0);

        expectedsize = roaring_bitmap_portable_size_in_bytes(r1);
        serializedbytes = (bytea *) palloc(VARHDRSZ + expectedsize);
        roaring_bitmap_portable_serialize(r1, VARDATA(serializedbytes));

        SET_VARSIZE(serializedbytes, VARHDRSZ + expectedsize);
        PG_RETURN_BYTEA_P(serializedbytes);
    }
}

//bitmap Deserialize
PG_FUNCTION_INFO_V1(rb_deserialize);
Datum rb_deserialize(PG_FUNCTION_ARGS);

Datum
rb_deserialize(PG_FUNCTION_ARGS) {
    MemoryContext aggctx;
    bytea *serializedbytes;
    roaring_bitmap_t *r1;

    // We must be called as a transition routine or we fail.
    if (!AggCheckCallContext(fcinfo, &aggctx))
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("rb_deserialize outside aggregate context")));

    // Is the first argument a NULL?
    if (PG_ARGISNULL(0)) {
        PG_RETURN_NULL();
    } else {
        serializedbytes = PG_GETARG_BYTEA_P(0);
        r1 = roaring_bitmap_portable_deserialize(VARDATA(serializedbytes));
        if (!r1)
            ereport(ERROR,
                    (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                     errmsg("bitmap format is error")));
        // PostgreSQL's combine_aggregates() only init fcinfo->isnull once,
        // set fcinfo->isnull here to avoid bug https://github.com/ChenHuajun/pg_roaringbitmap/issues/6
        fcinfo->isnull = false;
        PG_RETURN_POINTER(r1);
    }
}

//bitmap Cardinality trans
PG_FUNCTION_INFO_V1(rb_cardinality_final);
Datum rb_cardinality_final(PG_FUNCTION_ARGS);

Datum
rb_cardinality_final(PG_FUNCTION_ARGS) {
    MemoryContext aggctx;
    roaring_bitmap_t *r1;
    uint64 card1;

    // We must be called as a transition routine or we fail.
    if (!AggCheckCallContext(fcinfo, &aggctx))
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("rb_cardinality_final outside aggregate context")));

    // Is the first argument a NULL?
    if (PG_ARGISNULL(0)) {
        PG_RETURN_NULL();
    } else {
        r1 = (roaring_bitmap_t *) PG_GETARG_POINTER(0);

        card1 = roaring_bitmap_get_cardinality(r1);

        PG_RETURN_INT64(card1);
    }
}
