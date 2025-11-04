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



// kmerge SRFs
PG_FUNCTION_INFO_V1(rb_kmerge);
Datum rb_kmerge(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(rb_kmerge_avg);
Datum rb_kmerge_avg(PG_FUNCTION_ARGS);

typedef struct {
    uint32 value;
    bool has_value;
} OptU32;

typedef struct {
    int element_idx; /* 0-based index of iterator */
    OptU32 value;
} KMergNode;

typedef struct {
    int n; /* number of iterators */
    roaring_uint32_iterator_t **iters;
    /* tournament tree internals */
    OptU32 *value_losertree; /* size nodecount */
    int *idx_losertree;      /* size nodecount */
    size_t nodecount;
    KMergNode winner;
    TupleDesc tupdesc; /* cached result tuple descriptor */
    int32 *labels; /* optional labels per iterator (for avg variant) */
} KMergeState;

static inline bool kmerge_cond_opt_greater(const OptU32 *candidate, const OptU32 *tree)
{
    if (candidate->has_value && tree->has_value)
        return candidate->value > tree->value;
    if (!candidate->has_value && !tree->has_value)
        return false;
    if (!tree->has_value)
        return false;
    if (!candidate->has_value)
        return true;
    return false;
}

static KMergNode kmerge_rebuild_loser_tree(size_t idx,
                                           OptU32 *value_tree,
                                           int *idx_tree,
                                           size_t total_len)
{
    size_t left_idx = 2 * idx + 1;
    size_t right_idx = 2 * idx + 2;

    if (right_idx >= total_len) {
        /* leaf */
        KMergNode leaf;
        leaf.value = value_tree[idx];
        leaf.element_idx = idx_tree[idx];
        return leaf;
    }

    KMergNode left = kmerge_rebuild_loser_tree(left_idx, value_tree, idx_tree, total_len);
    KMergNode right = kmerge_rebuild_loser_tree(right_idx, value_tree, idx_tree, total_len);

    if (!left.value.has_value && !right.value.has_value) {
        idx_tree[idx] = right.element_idx;
        value_tree[idx] = right.value;
        return left;
    } else if (!left.value.has_value && right.value.has_value) {
        idx_tree[idx] = left.element_idx;
        value_tree[idx] = left.value;
        return right;
    } else if (left.value.has_value && !right.value.has_value) {
        idx_tree[idx] = right.element_idx;
        value_tree[idx] = right.value;
        return left;
    } else {
        if (left.value.value < right.value.value) {
            idx_tree[idx] = right.element_idx;
            value_tree[idx] = right.value;
            return left;
        } else {
            idx_tree[idx] = left.element_idx;
            value_tree[idx] = left.value;
            return right;
        }
    }
}

static inline void kmerge_pop_and_push(KMergeState *state)
{
    if (!state->winner.value.has_value)
        return;

    roaring_uint32_iterator_t *it = state->iters[state->winner.element_idx];
    /* advance iterator corresponding to current winner */
    roaring_uint32_iterator_advance(it);

    int candidate_idx = state->winner.element_idx;
    OptU32 candidate_value;
    candidate_value.has_value = it->has_value;
    if (candidate_value.has_value)
        candidate_value.value = it->current_value;

    size_t current_idx = state->nodecount + (size_t)candidate_idx;
    while (current_idx >= 1) {
        current_idx = (current_idx - 1) >> 1;

        OptU32 tree_value = state->value_losertree[current_idx];
        int tree_idx = state->idx_losertree[current_idx];

        bool cond = kmerge_cond_opt_greater(&candidate_value, &tree_value);

        int new_candidate_idx, new_tree_idx;
        OptU32 new_candidate_value, new_tree_value;

        if (cond) {
            new_candidate_idx = tree_idx;
            new_tree_idx = candidate_idx;
            new_candidate_value = tree_value;
            new_tree_value = candidate_value;
        } else {
            new_candidate_idx = candidate_idx;
            new_tree_idx = tree_idx;
            new_candidate_value = candidate_value;
            new_tree_value = tree_value;
        }

        state->idx_losertree[current_idx] = new_tree_idx;
        state->value_losertree[current_idx] = new_tree_value;
        candidate_idx = new_candidate_idx;
        candidate_value = new_candidate_value;
    }

    state->winner.element_idx = candidate_idx;
    state->winner.value = candidate_value;
}

static int int32_asc_cmp(const void *a, const void *b)
{
    int32 aa = *(const int32 *)a;
    int32 bb = *(const int32 *)b;
    if (aa < bb) return -1;
    if (aa > bb) return 1;
    return 0;
}

Datum
rb_kmerge(PG_FUNCTION_ARGS)
{
    FuncCallContext *funcctx;
    MemoryContext oldcontext;

    if (SRF_IS_FIRSTCALL()) {
        ArrayType *arr = PG_GETARG_ARRAYTYPE_P(0);
        funcctx = SRF_FIRSTCALL_INIT();

        oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

        int16 elmlen;
        bool elmbyval;
        char elmalign;
        Oid elmtype = ARR_ELEMTYPE(arr);
        get_typlenbyvalalign(elmtype, &elmlen, &elmbyval, &elmalign);

        Datum *elem_values;
        bool *elem_nulls;
        int nelems;
        deconstruct_array(arr, elmtype, elmlen, elmbyval, elmalign,
                          &elem_values, &elem_nulls, &nelems);

        int count = 0;
        for (int i = 0; i < nelems; i++) {
            if (!elem_nulls[i]) count++;
        }

        KMergeState *state = (KMergeState *) palloc0(sizeof(KMergeState));
        state->n = count;
        state->iters = (roaring_uint32_iterator_t **) palloc0(sizeof(roaring_uint32_iterator_t *) * Max(count, 1));
        state->labels = NULL;

        /* Prepare leaves */
        size_t elementcount = (size_t) count;
        if (elementcount == 0) {
            state->nodecount = 0;
            state->winner.element_idx = -1;
            state->winner.value.has_value = false;
        } else {
            size_t nodecount = elementcount - 1;
            /* temp arrays: internal nodes + leaves */
            OptU32 *value_tree = (OptU32 *) palloc0(sizeof(OptU32) * (nodecount + elementcount));
            int *idx_tree = (int *) palloc0(sizeof(int) * (nodecount + elementcount));

            int out_idx = 0;
            for (int i = 0; i < nelems; i++) {
                if (elem_nulls[i]) continue;
                bytea *data = (bytea *) DatumGetPointer(elem_values[i]);
                roaring_bitmap_t *rb = roaring_bitmap_portable_deserialize(VARDATA(data));
                if (!rb)
                    ereport(ERROR,
                            (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                             errmsg("bitmap format is error")));
                roaring_uint32_iterator_t *it = roaring_iterator_create(rb);
                state->iters[out_idx] = it;
                idx_tree[nodecount + out_idx] = out_idx;
                if (it->has_value) {
                    value_tree[nodecount + out_idx].has_value = true;
                    value_tree[nodecount + out_idx].value = it->current_value;
                } else {
                    value_tree[nodecount + out_idx].has_value = false;
                }
                out_idx++;
            }

            /* initialize internal nodes */
            for (size_t i = 0; i < nodecount; i++) {
                value_tree[i].has_value = false;
                idx_tree[i] = -1;
            }

            KMergNode winner = kmerge_rebuild_loser_tree(0, value_tree, idx_tree, nodecount + elementcount);

            /* store losertree (internal nodes only) */
            state->value_losertree = (OptU32 *) palloc(sizeof(OptU32) * nodecount);
            state->idx_losertree = (int *) palloc(sizeof(int) * nodecount);
            for (size_t i = 0; i < nodecount; i++) {
                state->value_losertree[i] = value_tree[i];
                state->idx_losertree[i] = idx_tree[i];
            }
            state->nodecount = nodecount;
            state->winner = winner;

            pfree(value_tree);
            pfree(idx_tree);
        }

        /* set up result tuple descriptor */
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

    if (!state->winner.value.has_value) {
        /* cleanup */
        for (int i = 0; i < state->n; i++) {
            if (state->iters[i])
                roaring_uint32_iterator_free(state->iters[i]);
        }
        if (state->iters) pfree(state->iters);
        if (state->value_losertree) pfree(state->value_losertree);
        if (state->idx_losertree) pfree(state->idx_losertree);
        pfree(state);
        SRF_RETURN_DONE(funcctx);
    }

    /* collect all sources for the current minimum value */
    uint32 current_val_u = state->winner.value.value;
    int32 current_val = (int32) current_val_u;

    /* collect indices (0-based) */
    int32 *sources = (int32 *) palloc(sizeof(int32) * Max(state->n, 1));
    int nsources = 0;

    do {
        /* record source (convert to 1-based later) */
        sources[nsources++] = (int32) state->winner.element_idx;
        /* advance tree */
        kmerge_pop_and_push(state);
    } while (state->winner.value.has_value && state->winner.value.value == current_val_u);

    /* sort and convert to 1-based */
    qsort(sources, nsources, sizeof(int32), int32_asc_cmp);
    for (int i = 0; i < nsources; i++) sources[i] += 1;

    Datum vals[2];
    bool nulls[2] = {false, false};

    vals[0] = Int32GetDatum(current_val);

    Datum *src_datums = (Datum *) palloc(sizeof(Datum) * nsources);
    for (int i = 0; i < nsources; i++) src_datums[i] = Int32GetDatum(sources[i]);
    ArrayType *src_array = construct_array(src_datums, nsources, INT4OID, sizeof(int32), true, 'i');
    vals[1] = PointerGetDatum(src_array);

    HeapTuple tuple = heap_form_tuple(state->tupdesc, vals, nulls);
    SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
}

Datum
rb_kmerge_avg(PG_FUNCTION_ARGS)
{
    FuncCallContext *funcctx;
    MemoryContext oldcontext;

    if (SRF_IS_FIRSTCALL()) {
        ArrayType *arr_bitmaps = PG_GETARG_ARRAYTYPE_P(0);
        ArrayType *arr_labels = PG_GETARG_ARRAYTYPE_P(1);
        funcctx = SRF_FIRSTCALL_INIT();

        oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

        int16 blen; bool bbyval; char balign;
        Oid btype = ARR_ELEMTYPE(arr_bitmaps);
        get_typlenbyvalalign(btype, &blen, &bbyval, &balign);

        int16 llen; bool lbyval; char lalign;
        Oid ltype = ARR_ELEMTYPE(arr_labels);
        get_typlenbyvalalign(ltype, &llen, &lbyval, &lalign);

        Datum *bvals; bool *bnulls; int bnelems;
        deconstruct_array(arr_bitmaps, btype, blen, bbyval, balign,
                          &bvals, &bnulls, &bnelems);

        Datum *lvals; bool *lnulls; int lnelems;
        deconstruct_array(arr_labels, ltype, llen, lbyval, lalign,
                          &lvals, &lnulls, &lnelems);

        if (lnelems != bnelems)
            ereport(ERROR,
                    (errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
                     errmsg("labels array length must match bitmaps array length")));

        int count = 0;
        for (int i = 0; i < bnelems; i++) {
            if (!bnulls[i]) count++;
        }

        KMergeState *state = (KMergeState *) palloc0(sizeof(KMergeState));
        state->n = count;
        state->iters = (roaring_uint32_iterator_t **) palloc0(sizeof(roaring_uint32_iterator_t *) * Max(count, 1));
        state->labels = (int32 *) palloc0(sizeof(int32) * Max(count, 1));

        size_t elementcount = (size_t) count;
        if (elementcount == 0) {
            state->nodecount = 0;
            state->winner.element_idx = -1;
            state->winner.value.has_value = false;
        } else {
            size_t nodecount = elementcount - 1;
            OptU32 *value_tree = (OptU32 *) palloc0(sizeof(OptU32) * (nodecount + elementcount));
            int *idx_tree = (int *) palloc0(sizeof(int) * (nodecount + elementcount));

            int out_idx = 0;
            for (int i = 0; i < bnelems; i++) {
                if (bnulls[i]) continue;

                if (lnulls && lnulls[i])
                    ereport(ERROR,
                            (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                             errmsg("label value must not be NULL when corresponding bitmap is non-NULL")));

                bytea *data = (bytea *) DatumGetPointer(bvals[i]);
                roaring_bitmap_t *rb = roaring_bitmap_portable_deserialize(VARDATA(data));
                if (!rb)
                    ereport(ERROR,
                            (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                             errmsg("bitmap format is error")));

                roaring_uint32_iterator_t *it = roaring_iterator_create(rb);
                state->iters[out_idx] = it;
                state->labels[out_idx] = DatumGetInt32(lvals[i]);

                idx_tree[nodecount + out_idx] = out_idx;
                if (it->has_value) {
                    value_tree[nodecount + out_idx].has_value = true;
                    value_tree[nodecount + out_idx].value = it->current_value;
                } else {
                    value_tree[nodecount + out_idx].has_value = false;
                }
                out_idx++;
            }

            for (size_t i = 0; i < nodecount; i++) {
                value_tree[i].has_value = false;
                idx_tree[i] = -1;
            }

            KMergNode winner = kmerge_rebuild_loser_tree(0, value_tree, idx_tree, nodecount + elementcount);

            state->value_losertree = (OptU32 *) palloc(sizeof(OptU32) * nodecount);
            state->idx_losertree = (int *) palloc(sizeof(int) * nodecount);
            for (size_t i = 0; i < nodecount; i++) {
                state->value_losertree[i] = value_tree[i];
                state->idx_losertree[i] = idx_tree[i];
            }
            state->nodecount = nodecount;
            state->winner = winner;

            pfree(value_tree);
            pfree(idx_tree);
        }

        funcctx->user_fctx = state;
        MemoryContextSwitchTo(oldcontext);
    }

    funcctx = SRF_PERCALL_SETUP();
    KMergeState *state = (KMergeState *) funcctx->user_fctx;

    if (!state->winner.value.has_value) {
        for (int i = 0; i < state->n; i++) {
            if (state->iters[i])
                roaring_uint32_iterator_free(state->iters[i]);
        }
        if (state->iters) pfree(state->iters);
        if (state->labels) pfree(state->labels);
        if (state->value_losertree) pfree(state->value_losertree);
        if (state->idx_losertree) pfree(state->idx_losertree);
        pfree(state);
        SRF_RETURN_DONE(funcctx);
    }

    uint32 current_val_u = state->winner.value.value;

    double sum = 0.0;
    int cnt = 0;

    do {
        int src = state->winner.element_idx;
        sum += (double) state->labels[src];
        cnt++;
        kmerge_pop_and_push(state);
    } while (state->winner.value.has_value && state->winner.value.value == current_val_u);

    double avg = cnt > 0 ? (sum / (double) cnt) : 0.0;

    SRF_RETURN_NEXT(funcctx, Float8GetDatum(avg));
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
