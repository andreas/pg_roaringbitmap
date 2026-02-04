#include "roaringbitmap.h"
#include "utils/lsyscache.h"
#include "fmgr.h"
#include "funcapi.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/syscache.h"
#include "utils/memutils.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "catalog/pg_cast.h"
#include "nodes/execnodes.h"
#include "nodes/makefuncs.h"
#include "nodes/primnodes.h"

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
PG_FUNCTION_INFO_V1(rb_kmerge_agg);
Datum rb_kmerge_agg(PG_FUNCTION_ARGS);

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
    /* heap-based k-way merge */
    KMergNode *heap; /* min-heap of active iterators by current value */
    int heap_size;
    TupleDesc tupdesc; /* cached result tuple descriptor */
    int32 *labels; /* optional labels per iterator (for avg variant) */
} KMergeState;

/* kept for past loser-tree implementation; now unused */

/* --- Min-heap helpers for k-way merge --- */
static inline void heap_sift_down(KMergNode *heap, int size, int idx)
{
    for (;;) {
        int left = (idx << 1) + 1;
        if (left >= size) break;
        int right = left + 1;
        int smallest = left;
        if (right < size && heap[right].value.value < heap[left].value.value)
            smallest = right;
        if (!(heap[smallest].value.value < heap[idx].value.value))
            break;
        KMergNode tmp = heap[idx];
        heap[idx] = heap[smallest];
        heap[smallest] = tmp;
        idx = smallest;
    }
}

static inline void heap_heapify(KMergNode *heap, int size)
{
    for (int i = (size >> 1) - 1; i >= 0; i--) {
        heap_sift_down(heap, size, i);
    }
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

        /* Build heap of active iterators */
        state->heap = (KMergNode *) palloc(sizeof(KMergNode) * Max(count, 1));
        int heap_size = 0;
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
            if (it->has_value) {
                state->heap[heap_size].element_idx = out_idx;
                state->heap[heap_size].value.has_value = true;
                state->heap[heap_size].value.value = it->current_value;
                heap_size++;
            }
            out_idx++;
        }
        state->heap_size = heap_size;
        if (heap_size > 1) {
            heap_heapify(state->heap, heap_size);
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

    if (state->heap_size == 0) {
        /* cleanup */
        for (int i = 0; i < state->n; i++) {
            if (state->iters[i])
                roaring_uint32_iterator_free(state->iters[i]);
        }
        if (state->iters) pfree(state->iters);
        if (state->heap) pfree(state->heap);
        pfree(state);
        SRF_RETURN_DONE(funcctx);
    }

    /* collect all sources for the current minimum value */
    uint32 current_val_u = state->heap[0].value.value;
    int32 current_val = (int32) current_val_u;

    /* collect indices (0-based) */
    int32 *sources = (int32 *) palloc(sizeof(int32) * Max(state->n, 1));
    int nsources = 0;

    do {
        /* record source (convert to 1-based later) */
        int src = state->heap[0].element_idx;
        sources[nsources++] = (int32) src;
        /* advance iterator at heap root and adjust heap */
        roaring_uint32_iterator_t *it = state->iters[src];
        roaring_uint32_iterator_advance(it);
        if (it->has_value) {
            state->heap[0].value.value = it->current_value;
            heap_sift_down(state->heap, state->heap_size, 0);
        } else {
            /* remove root */
            state->heap[0] = state->heap[state->heap_size - 1];
            state->heap_size--;
            if (state->heap_size > 0)
                heap_sift_down(state->heap, state->heap_size, 0);
        }
    } while (state->heap_size > 0 && state->heap[0].value.value == current_val_u);

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

        state->heap = (KMergNode *) palloc(sizeof(KMergNode) * Max(count, 1));
        int heap_size = 0;
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

            if (it->has_value) {
                state->heap[heap_size].element_idx = out_idx;
                state->heap[heap_size].value.has_value = true;
                state->heap[heap_size].value.value = it->current_value;
                heap_size++;
            }
            out_idx++;
        }
        state->heap_size = heap_size;
        if (heap_size > 1)
            heap_heapify(state->heap, heap_size);

        funcctx->user_fctx = state;
        MemoryContextSwitchTo(oldcontext);
    }

    funcctx = SRF_PERCALL_SETUP();
    KMergeState *state = (KMergeState *) funcctx->user_fctx;

    if (state->heap_size == 0) {
        for (int i = 0; i < state->n; i++) {
            if (state->iters[i])
                roaring_uint32_iterator_free(state->iters[i]);
        }
        if (state->iters) pfree(state->iters);
        if (state->labels) pfree(state->labels);
        if (state->heap) pfree(state->heap);
        pfree(state);
        SRF_RETURN_DONE(funcctx);
    }

    uint32 current_val_u = state->heap[0].value.value;

    double sum = 0.0;
    int cnt = 0;

    do {
        int src = state->heap[0].element_idx;
        sum += (double) state->labels[src];
        cnt++;
        roaring_uint32_iterator_t *it = state->iters[src];
        roaring_uint32_iterator_advance(it);
        if (it->has_value) {
            state->heap[0].value.value = it->current_value;
            heap_sift_down(state->heap, state->heap_size, 0);
        } else {
            state->heap[0] = state->heap[state->heap_size - 1];
            state->heap_size--;
            if (state->heap_size > 0)
                heap_sift_down(state->heap, state->heap_size, 0);
        }
    } while (state->heap_size > 0 && state->heap[0].value.value == current_val_u);

    double avg = cnt > 0 ? (sum / (double) cnt) : 0.0;

    SRF_RETURN_NEXT(funcctx, Float8GetDatum(avg));
}

/*
 * rb_kmerge_agg(bitmaps roaringbitmap[], labels anyarray, agg regprocedure, resulttype anycompatible)
 * Returns TABLE(element int, agg_value anycompatible)
 *
 * For each element present across the k input bitmaps, collect the labels
 * from all bitmaps containing that element and compute the aggregation using
 * the provided aggregate (resolved via pg_aggregate). The aggregate must be a
 * normal one-argument aggregate (no ordered-set, no moving-aggregate extras).
 */
typedef struct {
    int n;
    roaring_uint32_iterator_t **iters;
    KMergNode *heap;
    int heap_size;

    /* labels (one per iterator, corresponding to non-NULL bitmap entries) */
    Datum *labels;
    Oid label_elem_type;

    /* type layout for labels->input conversion */
    int16 label_typlen;
    bool label_typbyval;
    char label_typalign;
    int16 input_typlen;
    bool input_typbyval;
    char input_typalign;

    /* aggregate resolution */
    Oid agg_oid;              /* pg_proc OID of aggregate function */
    Oid trans_type;           /* aggtranstype */
    int16 trans_typlen;       /* transtype length for datumCopy */
    bool trans_typbyval;      /* transtype pass-by-value for datumCopy */
    Oid input_type;           /* aggregate input argument type */
    Oid finalfn_oid;          /* optional */
    FmgrInfo transfn;
    FmgrInfo finalfn;         /* valid only if finalfn_oid != InvalidOid */
    bool has_finalfn;
    bool transfn_strict;      /* true if transfn is strict */

    /* init transition value, if specified */
    Datum init_trans_value;
    bool init_trans_isnull;

    /* IO helpers for label->input and (final)->result conversions */
    Oid label_out_func;
    Oid input_in_func;
    Oid input_in_ioparam;

    /* cast resolution for label->input */
    char label_cast_method;   /* 'b' binary, 'f' function, 'i' io, or 0 if same */
    FmgrInfo label_cast_fn;   /* valid iff label_cast_method == 'f' */

    Oid agg_result_type;      /* prorettype of aggregate function */
    Oid agg_result_out_func;
    Oid result_in_func;       /* for requested anycompatible result type */
    Oid result_in_ioparam;

    /* cast resolution for agg_result -> requested result */
    char result_cast_method;  /* 'b', 'f', 'i', or 0 if same */
    FmgrInfo result_cast_fn;  /* valid iff result_cast_method == 'f' */

    Oid result_type;          /* resolved anycompatible */

    MemoryContext pergroup_ctx;

    /* Fake AggState for AggCheckCallContext support (internal transtype aggs) */
    AggState *fake_aggstate;
    ExprContext *fake_aggcontext;
} KMergeAggState;

static Datum
rb_coerce_datum_via_io(Datum value, Oid from_out_func, Oid to_in_func, Oid to_in_ioparam)
{
    char *tmp = OidOutputFunctionCall(from_out_func, value);
    Datum res = OidInputFunctionCall(to_in_func, tmp, to_in_ioparam, -1);
    pfree(tmp);
    return res;
}

/*
 * Apply a resolved cast to a datum.
 * cast_method: 0 = same type, 'b' = binary compatible, 'f' = function, 'i' = IO
 */
static Datum
rb_apply_cast(Datum value, char cast_method, FmgrInfo *cast_fn,
              Oid from_out_func, Oid to_in_func, Oid to_in_ioparam)
{
    switch (cast_method) {
        case 0:   /* same type */
        case 'b': /* binary compatible */
            return value;
        case 'f': /* cast function */
            return FunctionCall1(cast_fn, value);
        case 'i': /* IO conversion */
        default:
            return rb_coerce_datum_via_io(value, from_out_func, to_in_func, to_in_ioparam);
    }
}

/* resolve cast method between two types via pg_cast; returns method char or 0 if same */
static char
rb_resolve_cast_method(Oid fromtype, Oid totype, Oid *funcOid)
{
    if (fromtype == totype) {
        if (funcOid) *funcOid = InvalidOid;
        return 0; /* same type */
    }

    HeapTuple tup = SearchSysCache2(CASTSOURCETARGET,
                                    ObjectIdGetDatum(fromtype),
                                    ObjectIdGetDatum(totype));
    if (!HeapTupleIsValid(tup)) {
        if (funcOid) *funcOid = InvalidOid;
        return 'i'; /* fallback to IO if no explicit cast */
    }
    Form_pg_cast castForm = (Form_pg_cast) GETSTRUCT(tup);
    char method = castForm->castmethod; /* 'b', 'f', or 'i' */
    if (funcOid) *funcOid = castForm->castfunc;
    ReleaseSysCache(tup);
    return method;
}

Datum
rb_kmerge_agg(PG_FUNCTION_ARGS)
{
    FuncCallContext *funcctx;
    MemoryContext oldcontext;

    if (SRF_IS_FIRSTCALL()) {
        ArrayType *arr_bitmaps = PG_GETARG_ARRAYTYPE_P(0);
        ArrayType *arr_labels = PG_GETARG_ARRAYTYPE_P(1);
        Oid agg_oid = PG_GETARG_OID(2);
        funcctx = SRF_FIRSTCALL_INIT();

        oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

        /* Prepare bitmap array deconstruction */
        int16 blen; bool bbyval; char balign;
        Oid btype = ARR_ELEMTYPE(arr_bitmaps);
        get_typlenbyvalalign(btype, &blen, &bbyval, &balign);

        /* Labels array */
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

        /* Build iterators for non-NULL bitmaps */
        int count = 0;
        for (int i = 0; i < bnelems; i++) {
            if (!bnulls[i]) count++;
        }

        KMergeAggState *state = (KMergeAggState *) palloc0(sizeof(KMergeAggState));
        state->n = count;
        state->iters = (roaring_uint32_iterator_t **) palloc0(sizeof(roaring_uint32_iterator_t *) * Max(count, 1));
        state->labels = (Datum *) palloc0(sizeof(Datum) * Max(count, 1));
        state->label_elem_type = ltype;

        state->heap = (KMergNode *) palloc(sizeof(KMergNode) * Max(count, 1));
        int heap_size = 0;
        int out_idx = 0;

        /* --- Resolve aggregate input/result types early for label coercion --- */
        /* inspect aggregate pg_proc to get input and result types */
        HeapTuple procTup_e = SearchSysCache1(PROCOID, ObjectIdGetDatum(agg_oid));
        if (!HeapTupleIsValid(procTup_e))
            ereport(ERROR,
                    (errcode(ERRCODE_UNDEFINED_FUNCTION),
                     errmsg("could not find pg_proc row for aggregate %u", agg_oid)));
        Form_pg_proc procForm_e = (Form_pg_proc) GETSTRUCT(procTup_e);
        if (procForm_e->pronargs != 1)
        {
            Oid nsp = procForm_e->pronamespace;
            char *nspname = get_namespace_name(nsp);
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
                     errmsg("aggregate %s.%s must take exactly one argument",
                            nspname ? nspname : "", NameStr(procForm_e->proname))));
        }
        state->input_type = procForm_e->proargtypes.values[0];
        state->agg_result_type = procForm_e->prorettype;

        /* For polymorphic aggregates (e.g., array_agg(anynonarray)),
         * substitute the actual type from the labels array */
        if (IsPolymorphicType(state->input_type))
            state->input_type = state->label_elem_type;

        /* For polymorphic result types (e.g., anyarray from array_agg),
         * we'll rely on the output column definition from RETURNS record.
         * Set agg_result_type to the user-specified result_type later. */

        ReleaseSysCache(procTup_e);

        /* type layout for copy */
        get_typlenbyvalalign(state->label_elem_type, &state->label_typlen, &state->label_typbyval, &state->label_typalign);
        get_typlenbyvalalign(state->input_type, &state->input_typlen, &state->input_typbyval, &state->input_typalign);

        /* resolve label->input cast path */
        Oid label_cast_func = InvalidOid;
        state->label_cast_method = rb_resolve_cast_method(state->label_elem_type, state->input_type, &label_cast_func);
        if (state->label_cast_method == 'f')
            fmgr_info_cxt(label_cast_func, &state->label_cast_fn, funcctx->multi_call_memory_ctx);

        /* IO helpers for label->input conversion (used if cast method is 'i') */
        {
            bool typIsVarlena;
            getTypeOutputInfo(state->label_elem_type, &state->label_out_func, &typIsVarlena);
            getTypeInputInfo(state->input_type, &state->input_in_func, &state->input_in_ioparam);
        }

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
            /* pre-coerce label to aggregate input type once */
            {
                Datum coerced = rb_apply_cast(lvals[i], state->label_cast_method,
                                              &state->label_cast_fn,
                                              state->label_out_func,
                                              state->input_in_func,
                                              state->input_in_ioparam);
                /* copy to multi-call context lifetime */
                state->labels[out_idx] = datumCopy(coerced, state->input_typbyval, state->input_typlen);
            }

            if (it->has_value) {
                state->heap[heap_size].element_idx = out_idx;
                state->heap[heap_size].value.has_value = true;
                state->heap[heap_size].value.value = it->current_value;
                heap_size++;
            }
            out_idx++;
        }
        state->heap_size = heap_size;
        if (heap_size > 1)
            heap_heapify(state->heap, heap_size);

        /* resolve aggregate */
        HeapTuple aggTup = SearchSysCache1(AGGFNOID, ObjectIdGetDatum(agg_oid));
        if (!HeapTupleIsValid(aggTup))
            ereport(ERROR,
                    (errcode(ERRCODE_UNDEFINED_FUNCTION),
                     errmsg("could not find aggregate with OID %u", agg_oid)));

        Form_pg_aggregate aggForm = (Form_pg_aggregate) GETSTRUCT(aggTup);
        state->agg_oid = agg_oid;
        state->trans_type = aggForm->aggtranstype;
        state->finalfn_oid = aggForm->aggfinalfn;
        state->has_finalfn = OidIsValid(state->finalfn_oid);

        /* Get transtype layout for datumCopy */
        get_typlenbyval(state->trans_type, &state->trans_typlen, &state->trans_typbyval);

        fmgr_info_cxt(aggForm->aggtransfn, &state->transfn, funcctx->multi_call_memory_ctx);
        state->transfn_strict = state->transfn.fn_strict;

        /* For polymorphic aggregates (e.g., array_agg), the transition function
         * calls get_fn_expr_argtype() to determine the actual input type.
         * We need to set up fn_expr with a FuncExpr that has the resolved types.
         */
        {
            FuncExpr *fexpr = makeNode(FuncExpr);
            fexpr->funcid = aggForm->aggtransfn;
            fexpr->funcresulttype = state->trans_type;
            fexpr->funcretset = false;
            fexpr->funcvariadic = false;
            fexpr->funcformat = COERCE_EXPLICIT_CALL;
            fexpr->funccollid = InvalidOid;
            fexpr->inputcollid = InvalidOid;
            fexpr->location = -1;
            /* Build args list with Const nodes of the correct types.
             * arg0 = trans_type (internal/state), arg1 = input_type (the actual element type) */
            Const *arg0 = makeConst(state->trans_type, -1, InvalidOid, -1,
                                    (Datum) 0, true, false);
            Const *arg1 = makeConst(state->input_type, -1, InvalidOid, -1,
                                    (Datum) 0, true, false);
            fexpr->args = list_make2(arg0, arg1);
            state->transfn.fn_expr = (Node *) fexpr;
        }

        if (state->has_finalfn)
            fmgr_info_cxt(state->finalfn_oid, &state->finalfn, funcctx->multi_call_memory_ctx);

        /* initval (text) -> Datum of transtype */
        bool isnull;
        Datum inittext = SysCacheGetAttr(AGGFNOID, aggTup, Anum_pg_aggregate_agginitval, &isnull);
        state->init_trans_isnull = true;
        if (!isnull) {
            Oid in_func; Oid ioparam;
            getTypeInputInfo(state->trans_type, &in_func, &ioparam);
            char *cstr = TextDatumGetCString(inittext);
            state->init_trans_value = OidInputFunctionCall(in_func, cstr, ioparam, -1);
            state->init_trans_isnull = false;
            pfree(cstr);
        }
        ReleaseSysCache(aggTup);

        /* input/result types already resolved above */

        /* IO helpers */
        /* get result type from output column definition (RETURNS record) */
        TupleDesc tupdesc;
        if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("function returning record called in context that cannot accept type record")));
        if (tupdesc->natts != 1)
            ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                     errmsg("rb_kmerge_agg expects exactly one output column")));
        state->result_type = TupleDescAttr(tupdesc, 0)->atttypid;
        getTypeInputInfo(state->result_type, &state->result_in_func, &state->result_in_ioparam);
        /* save tupdesc for building result tuples */
        funcctx->tuple_desc = BlessTupleDesc(tupdesc);

        /* For polymorphic result types (e.g., anyarray from array_agg),
         * use the user-specified result_type */
        if (IsPolymorphicType(state->agg_result_type))
            state->agg_result_type = state->result_type;

        /* IO helper for agg_result -> result conversion (deferred until agg_result_type is resolved) */
        {
            bool typIsVarlena;
            getTypeOutputInfo(state->agg_result_type, &state->agg_result_out_func, &typIsVarlena);
        }

        /* resolve agg_result -> requested result cast once */
        {
            Oid res_cast_func = InvalidOid;
            state->result_cast_method = rb_resolve_cast_method(state->agg_result_type, state->result_type, &res_cast_func);
            if (state->result_cast_method == 'f')
                fmgr_info_cxt(res_cast_func, &state->result_cast_fn, funcctx->multi_call_memory_ctx);
        }

        state->pergroup_ctx = AllocSetContextCreate(funcctx->multi_call_memory_ctx,
                                                    "rb_kmerge_agg pergroup",
                                                    ALLOCSET_DEFAULT_SIZES);

        /* Set up fake AggState for AggCheckCallContext support.
         * This is needed for aggregates with internal transtype (like array_agg).
         * We create a minimal AggState with just enough fields set for
         * AggCheckCallContext() to work properly.
         */
        state->fake_aggstate = (AggState *) palloc0(sizeof(AggState));
        state->fake_aggstate->ss.ps.type = T_AggState;  /* Make IsA(node, AggState) work */

        /* Create a fake ExprContext with the memory context for aggregate state */
        state->fake_aggcontext = (ExprContext *) palloc0(sizeof(ExprContext));
        state->fake_aggcontext->ecxt_per_tuple_memory = state->pergroup_ctx;

        /* Point curaggcontext to our fake ExprContext */
        state->fake_aggstate->curaggcontext = state->fake_aggcontext;

        funcctx->user_fctx = state;
        MemoryContextSwitchTo(oldcontext);
    }

    funcctx = SRF_PERCALL_SETUP();
    KMergeAggState *state = (KMergeAggState *) funcctx->user_fctx;

    if (state->heap_size == 0) {
        for (int i = 0; i < state->n; i++) {
            if (state->iters[i])
                roaring_uint32_iterator_free(state->iters[i]);
        }
        if (state->pergroup_ctx)
            MemoryContextDelete(state->pergroup_ctx);
        /* remaining allocations (iters, labels, heap, state) live in multi_call_memory_ctx
         * and will be freed automatically when the SRF completes */
        SRF_RETURN_DONE(funcctx);
    }

    /* Save original context and switch to per-group context for aggregate computation */
    MemoryContext agg_ctx = MemoryContextSwitchTo(state->pergroup_ctx);

    /* current element value */
    uint32 current_val_u = state->heap[0].value.value;

    /* aggregate across all sources matching current value */
    /* Make a fresh copy of init value for this group to avoid modifying the original */
    Datum trans = (Datum) 0;
    bool trans_isnull = true;
    if (!state->init_trans_isnull) {
        trans = datumCopy(state->init_trans_value, state->trans_typbyval, state->trans_typlen);
        trans_isnull = false;
    }

    do {
        int src = state->heap[0].element_idx;

        /* use pre-coerced input label */
        Datum input_val = state->labels[src];

        /* For strict transition functions with NULL trans, the first input becomes
         * the new trans value directly (skipping the function call). This matches
         * PostgreSQL's aggregate semantics for aggregates like min/max without initval. */
        if (state->transfn_strict && trans_isnull) {
            trans = datumCopy(input_val, state->input_typbyval, state->input_typlen);
            trans_isnull = false;
        } else {
            LOCAL_FCINFO(fcinfo_trans, 2);
            /* Set context to fake AggState for AggCheckCallContext support */
            InitFunctionCallInfoData(*fcinfo_trans, &state->transfn, 2, InvalidOid,
                                     (Node *) state->fake_aggstate, NULL);
            fcinfo_trans->args[0].value = trans;
            fcinfo_trans->args[0].isnull = trans_isnull;
            fcinfo_trans->args[1].value = input_val;
            fcinfo_trans->args[1].isnull = false;
            Datum new_trans = FunctionCallInvoke(fcinfo_trans);
            trans = new_trans;
            trans_isnull = fcinfo_trans->isnull;
        }

        /* advance iterator */
        roaring_uint32_iterator_t *it = state->iters[src];
        roaring_uint32_iterator_advance(it);
        if (it->has_value) {
            state->heap[0].value.value = it->current_value;
            heap_sift_down(state->heap, state->heap_size, 0);
        } else {
            state->heap[0] = state->heap[state->heap_size - 1];
            state->heap_size--;
            if (state->heap_size > 0)
                heap_sift_down(state->heap, state->heap_size, 0);
        }
    } while (state->heap_size > 0 && state->heap[0].value.value == current_val_u);

    /* finalize */
    Datum final_val;
    bool final_isnull;
    if (state->has_finalfn) {
        LOCAL_FCINFO(fcinfo_final, 1);
        /* Set context to fake AggState for AggCheckCallContext support */
        InitFunctionCallInfoData(*fcinfo_final, &state->finalfn, 1, InvalidOid,
                                 (Node *) state->fake_aggstate, NULL);
        fcinfo_final->args[0].value = trans;
        fcinfo_final->args[0].isnull = trans_isnull;
        final_val = FunctionCallInvoke(fcinfo_final);
        final_isnull = fcinfo_final->isnull;
    } else {
        final_val = trans;
        final_isnull = trans_isnull;
    }

    /* convert final value to requested result_type if needed, in multi-call ctx */
    MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
    Datum out_val = (Datum) 0;
    bool out_isnull = final_isnull;
    if (!final_isnull) {
        out_val = rb_apply_cast(final_val, state->result_cast_method,
                                &state->result_cast_fn,
                                state->agg_result_out_func,
                                state->result_in_func,
                                state->result_in_ioparam);
    }

    /* reset per-group allocations to avoid leaks */
    MemoryContextReset(state->pergroup_ctx);

    /* restore original context before returning */
    MemoryContextSwitchTo(agg_ctx);

    /* build result tuple */
    Datum values[1];
    bool nulls[1];
    values[0] = out_val;
    nulls[0] = out_isnull;
    HeapTuple tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
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
	roaring_bitmap_t *r1;
	roaring_bitmap_t *r2;
	uint64 card1;

	r1 = roaring_bitmap_portable_deserialize_safe(VARDATA(serializedbytes1),
	                                              VARSIZE(serializedbytes1) - VARHDRSZ);
	if (!r1)
		ereport(ERROR,
		        (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
		         errmsg("bitmap format is error")));

	r2 = roaring_bitmap_portable_deserialize_safe(VARDATA(serializedbytes2),
	                                              VARSIZE(serializedbytes2) - VARHDRSZ);
	if (!r2) {
		roaring_bitmap_free(r1);
		ereport(ERROR,
		        (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
		         errmsg("bitmap format is error")));
	}

	card1 = roaring_bitmap_or_cardinality(r1, r2);
	roaring_bitmap_free(r1);
	roaring_bitmap_free(r2);

	PG_RETURN_INT64(card1);
}

//bitmap_and
PG_FUNCTION_INFO_V1(rb_and);
Datum rb_and(PG_FUNCTION_ARGS);

Datum
rb_and(PG_FUNCTION_ARGS) {
    bytea *serializedbytes1 = PG_GETARG_BYTEA_P(0);
    bytea *serializedbytes2 = PG_GETARG_BYTEA_P(1);
	roaring_bitmap_t *r1;
	roaring_bitmap_t *r2;
	roaring_bitmap_t *r;
    size_t expectedsize;
    bytea *serializedbytes;

	r1 = roaring_bitmap_portable_deserialize_safe(VARDATA(serializedbytes1),
	                                              VARSIZE(serializedbytes1) - VARHDRSZ);
    if (!r1)
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));

	r2 = roaring_bitmap_portable_deserialize_safe(VARDATA(serializedbytes2),
	                                              VARSIZE(serializedbytes2) - VARHDRSZ);
    if (!r2) {
		roaring_bitmap_free(r1);
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));
    }

	r = roaring_bitmap_and(r1, r2);
	roaring_bitmap_free(r1);
	roaring_bitmap_free(r2);
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
	roaring_bitmap_t *r1;
	roaring_bitmap_t *r2;
	uint64 card1;

	r1 = roaring_bitmap_portable_deserialize_safe(VARDATA(serializedbytes1),
	                                              VARSIZE(serializedbytes1) - VARHDRSZ);
	if (!r1)
		ereport(ERROR,
		        (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
		         errmsg("bitmap format is error")));

	r2 = roaring_bitmap_portable_deserialize_safe(VARDATA(serializedbytes2),
	                                              VARSIZE(serializedbytes2) - VARHDRSZ);
	if (!r2) {
		roaring_bitmap_free(r1);
		ereport(ERROR,
		        (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
		         errmsg("bitmap format is error")));
	}

	card1 = roaring_bitmap_and_cardinality(r1, r2);
	roaring_bitmap_free(r1);
	roaring_bitmap_free(r2);

	PG_RETURN_INT64(card1);
}


//bitmap_andnot
PG_FUNCTION_INFO_V1(rb_andnot);
Datum rb_andnot(PG_FUNCTION_ARGS);

Datum
rb_andnot(PG_FUNCTION_ARGS) {
    bytea *serializedbytes1 = PG_GETARG_BYTEA_P(0);
    bytea *serializedbytes2 = PG_GETARG_BYTEA_P(1);
	roaring_bitmap_t *r1;
	roaring_bitmap_t *r2;
	roaring_bitmap_t *r;
    size_t expectedsize;
    bytea *serializedbytes;

	r1 = roaring_bitmap_portable_deserialize_safe(VARDATA(serializedbytes1),
	                                              VARSIZE(serializedbytes1) - VARHDRSZ);
    if (!r1)
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));

	r2 = roaring_bitmap_portable_deserialize_safe(VARDATA(serializedbytes2),
	                                              VARSIZE(serializedbytes2) - VARHDRSZ);
    if (!r2) {
		roaring_bitmap_free(r1);
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));
    }

	r = roaring_bitmap_andnot(r1, r2);
	roaring_bitmap_free(r1);
	roaring_bitmap_free(r2);
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
	roaring_bitmap_t *r1;
	roaring_bitmap_t *r2;
	uint64 card1;

	r1 = roaring_bitmap_portable_deserialize_safe(VARDATA(serializedbytes1),
	                                              VARSIZE(serializedbytes1) - VARHDRSZ);
	if (!r1)
		ereport(ERROR,
		        (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
		         errmsg("bitmap format is error")));

	r2 = roaring_bitmap_portable_deserialize_safe(VARDATA(serializedbytes2),
	                                              VARSIZE(serializedbytes2) - VARHDRSZ);
	if (!r2) {
		roaring_bitmap_free(r1);
		ereport(ERROR,
		        (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
		         errmsg("bitmap format is error")));
	}

	card1 = roaring_bitmap_andnot_cardinality(r1, r2);
	roaring_bitmap_free(r1);
	roaring_bitmap_free(r2);

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
	roaring_bitmap_t *r1;
	roaring_bitmap_t *r2;
	uint64 card1;

	r1 = roaring_bitmap_portable_deserialize_safe(VARDATA(serializedbytes1),
	                                              VARSIZE(serializedbytes1) - VARHDRSZ);
	if (!r1)
		ereport(ERROR,
		        (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
		         errmsg("bitmap format is error")));

	r2 = roaring_bitmap_portable_deserialize_safe(VARDATA(serializedbytes2),
	                                              VARSIZE(serializedbytes2) - VARHDRSZ);
	if (!r2) {
		roaring_bitmap_free(r1);
		ereport(ERROR,
		        (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
		         errmsg("bitmap format is error")));
	}

	card1 = roaring_bitmap_xor_cardinality(r1, r2);
	roaring_bitmap_free(r1);
	roaring_bitmap_free(r2);

	PG_RETURN_INT64(card1);
}


//bitmap cardinality
PG_FUNCTION_INFO_V1(rb_cardinality);
Datum rb_cardinality(PG_FUNCTION_ARGS);

Datum
rb_cardinality(PG_FUNCTION_ARGS) {
    bytea *data = PG_GETARG_BYTEA_P(0);
	roaring_bitmap_t *r1;
    uint64 card;

	r1 = roaring_bitmap_portable_deserialize_safe(VARDATA(data),
	                                              VARSIZE(data) - VARHDRSZ);
    if (!r1)
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));

	card = roaring_bitmap_get_cardinality(r1);
	roaring_bitmap_free(r1);

    PG_RETURN_INT64(card);
}


//bitmap is empty
PG_FUNCTION_INFO_V1(rb_is_empty);
Datum rb_is_empty(PG_FUNCTION_ARGS);

Datum
rb_is_empty(PG_FUNCTION_ARGS) {
    bytea *data = PG_GETARG_BYTEA_P(0);
	roaring_bitmap_t *r1;
	bool isempty;

	r1 = roaring_bitmap_portable_deserialize_safe(VARDATA(data),
	                                              VARSIZE(data) - VARHDRSZ);
    if (!r1)
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));

	isempty = roaring_bitmap_is_empty(r1);
	roaring_bitmap_free(r1);

	PG_RETURN_BOOL(isempty);
}

//bitmap contains one value
PG_FUNCTION_INFO_V1(rb_exsit);
Datum rb_exsit(PG_FUNCTION_ARGS);

Datum
rb_exsit(PG_FUNCTION_ARGS) {
    bytea *data = PG_GETARG_BYTEA_P(0);
    uint32 value = PG_GETARG_UINT32(1);
	roaring_bitmap_t *r1;
	bool isexsit;

	r1 = roaring_bitmap_portable_deserialize_safe(VARDATA(data),
	                                              VARSIZE(data) - VARHDRSZ);
    if (!r1)
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));

	isexsit = roaring_bitmap_contains(r1, value);
	roaring_bitmap_free(r1);

    PG_RETURN_BOOL(isexsit);
}

//bitmap equals
PG_FUNCTION_INFO_V1(rb_equals);
Datum rb_equals(PG_FUNCTION_ARGS);

Datum
rb_equals(PG_FUNCTION_ARGS) {
    bytea *serializedbytes1 = PG_GETARG_BYTEA_P(0);
    bytea *serializedbytes2 = PG_GETARG_BYTEA_P(1);
	roaring_bitmap_t *r1;
	roaring_bitmap_t *r2;
	bool isequal;

	r1 = roaring_bitmap_portable_deserialize_safe(VARDATA(serializedbytes1),
	                                              VARSIZE(serializedbytes1) - VARHDRSZ);
	if (!r1)
		ereport(ERROR,
		        (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
		         errmsg("bitmap format is error")));

	r2 = roaring_bitmap_portable_deserialize_safe(VARDATA(serializedbytes2),
	                                              VARSIZE(serializedbytes2) - VARHDRSZ);
	if (!r2) {
		roaring_bitmap_free(r1);
		ereport(ERROR,
		        (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
		         errmsg("bitmap format is error")));
	}

	isequal = roaring_bitmap_equals(r1, r2);
	roaring_bitmap_free(r1);
	roaring_bitmap_free(r2);

    PG_RETURN_BOOL(isequal);
}

//bitmap not equals
PG_FUNCTION_INFO_V1(rb_not_equals);
Datum rb_not_equals(PG_FUNCTION_ARGS);

Datum
rb_not_equals(PG_FUNCTION_ARGS) {
    bytea *serializedbytes1 = PG_GETARG_BYTEA_P(0);
    bytea *serializedbytes2 = PG_GETARG_BYTEA_P(1);
	roaring_bitmap_t *r1;
	roaring_bitmap_t *r2;
	bool isequal;

	r1 = roaring_bitmap_portable_deserialize_safe(VARDATA(serializedbytes1),
	                                              VARSIZE(serializedbytes1) - VARHDRSZ);
	if (!r1)
		ereport(ERROR,
		        (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
		         errmsg("bitmap format is error")));

	r2 = roaring_bitmap_portable_deserialize_safe(VARDATA(serializedbytes2),
	                                              VARSIZE(serializedbytes2) - VARHDRSZ);
	if (!r2) {
		roaring_bitmap_free(r1);
		ereport(ERROR,
		        (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
		         errmsg("bitmap format is error")));
	}

	isequal = roaring_bitmap_equals(r1, r2);
	roaring_bitmap_free(r1);
	roaring_bitmap_free(r2);

    PG_RETURN_BOOL(!isequal);
}

//bitmap intersect
PG_FUNCTION_INFO_V1(rb_intersect);
Datum rb_intersect(PG_FUNCTION_ARGS);

Datum
rb_intersect(PG_FUNCTION_ARGS) {
    bytea *serializedbytes1 = PG_GETARG_BYTEA_P(0);
    bytea *serializedbytes2 = PG_GETARG_BYTEA_P(1);
	roaring_bitmap_t *r1;
	roaring_bitmap_t *r2;
	bool isintersect;

	r1 = roaring_bitmap_portable_deserialize_safe(VARDATA(serializedbytes1),
	                                              VARSIZE(serializedbytes1) - VARHDRSZ);
	if (!r1)
		ereport(ERROR,
		        (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
		         errmsg("bitmap format is error")));

	r2 = roaring_bitmap_portable_deserialize_safe(VARDATA(serializedbytes2),
	                                              VARSIZE(serializedbytes2) - VARHDRSZ);
	if (!r2) {
		roaring_bitmap_free(r1);
		ereport(ERROR,
		        (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
		         errmsg("bitmap format is error")));
	}

	isintersect = roaring_bitmap_intersect(r1, r2);
	roaring_bitmap_free(r1);
	roaring_bitmap_free(r2);

    PG_RETURN_BOOL(isintersect);
}

//bitmap contains
PG_FUNCTION_INFO_V1(rb_contains);
Datum rb_contains(PG_FUNCTION_ARGS);

Datum
rb_contains(PG_FUNCTION_ARGS) {
    bytea *serializedbytes1 = PG_GETARG_BYTEA_P(0);
    bytea *serializedbytes2 = PG_GETARG_BYTEA_P(1);
	roaring_bitmap_t *r1;
	roaring_bitmap_t *r2;
	bool iscontain;

	r1 = roaring_bitmap_portable_deserialize_safe(VARDATA(serializedbytes1),
	                                              VARSIZE(serializedbytes1) - VARHDRSZ);
	if (!r1)
		ereport(ERROR,
		        (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
		         errmsg("bitmap format is error")));

	r2 = roaring_bitmap_portable_deserialize_safe(VARDATA(serializedbytes2),
	                                              VARSIZE(serializedbytes2) - VARHDRSZ);
	if (!r2) {
		roaring_bitmap_free(r1);
		ereport(ERROR,
		        (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
		         errmsg("bitmap format is error")));
	}

	iscontain = roaring_bitmap_is_subset(r2, r1);
	roaring_bitmap_free(r1);
	roaring_bitmap_free(r2);

    PG_RETURN_BOOL(iscontain);
}

//bitmap contained
PG_FUNCTION_INFO_V1(rb_containedby);
Datum rb_containedby(PG_FUNCTION_ARGS);

Datum
rb_containedby(PG_FUNCTION_ARGS) {
    bytea *serializedbytes1 = PG_GETARG_BYTEA_P(0);
    bytea *serializedbytes2 = PG_GETARG_BYTEA_P(1);
	roaring_bitmap_t *r1;
	roaring_bitmap_t *r2;
	bool iscontained;

	r1 = roaring_bitmap_portable_deserialize_safe(VARDATA(serializedbytes1),
	                                              VARSIZE(serializedbytes1) - VARHDRSZ);
	if (!r1)
		ereport(ERROR,
		        (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
		         errmsg("bitmap format is error")));

	r2 = roaring_bitmap_portable_deserialize_safe(VARDATA(serializedbytes2),
	                                              VARSIZE(serializedbytes2) - VARHDRSZ);
	if (!r2) {
		roaring_bitmap_free(r1);
		ereport(ERROR,
		        (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
		         errmsg("bitmap format is error")));
	}

	iscontained = roaring_bitmap_is_subset(r1, r2);
	roaring_bitmap_free(r1);
	roaring_bitmap_free(r2);

    PG_RETURN_BOOL(iscontained);
}

//bitmap jaccard distance
PG_FUNCTION_INFO_V1(rb_jaccard_dist);
Datum rb_jaccard_dist(PG_FUNCTION_ARGS);

Datum
rb_jaccard_dist(PG_FUNCTION_ARGS) {
    bytea *serializedbytes1 = PG_GETARG_BYTEA_P(0);
    bytea *serializedbytes2 = PG_GETARG_BYTEA_P(1);
	roaring_bitmap_t *r1;
	roaring_bitmap_t *r2;
	double jaccard_dist;

	r1 = roaring_bitmap_portable_deserialize_safe(VARDATA(serializedbytes1),
	                                              VARSIZE(serializedbytes1) - VARHDRSZ);
	if (!r1)
		ereport(ERROR,
		        (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
		         errmsg("bitmap format is error")));

	r2 = roaring_bitmap_portable_deserialize_safe(VARDATA(serializedbytes2),
	                                              VARSIZE(serializedbytes2) - VARHDRSZ);
	if (!r2) {
		roaring_bitmap_free(r1);
		ereport(ERROR,
		        (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
		         errmsg("bitmap format is error")));
	}

	jaccard_dist = roaring_bitmap_jaccard_index(r1, r2);
	roaring_bitmap_free(r1);
	roaring_bitmap_free(r2);

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
	roaring_bitmap_t *r1;
    uint32 min;

	r1 = roaring_bitmap_portable_deserialize_safe(VARDATA(data),
	                                              VARSIZE(data) - VARHDRSZ);
    if (!r1)
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));

	if(roaring_bitmap_is_empty(r1))
    {
		roaring_bitmap_free(r1);
        PG_RETURN_NULL();
    }

	min = roaring_bitmap_minimum(r1);
	roaring_bitmap_free(r1);

    PG_RETURN_UINT32(min);
}


//bitmap maximum
PG_FUNCTION_INFO_V1(rb_max);
Datum rb_max(PG_FUNCTION_ARGS);

Datum
rb_max(PG_FUNCTION_ARGS) {
    bytea *data = PG_GETARG_BYTEA_P(0);
	roaring_bitmap_t *r1;
    uint32 max;

	r1 = roaring_bitmap_portable_deserialize_safe(VARDATA(data),
	                                              VARSIZE(data) - VARHDRSZ);
    if (!r1)
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));

	if(roaring_bitmap_is_empty(r1))
    {
		roaring_bitmap_free(r1);
        PG_RETURN_NULL();
    }

	max = roaring_bitmap_maximum(r1);
	roaring_bitmap_free(r1);

    PG_RETURN_UINT32(max);
}

//bitmap rank
PG_FUNCTION_INFO_V1(rb_rank);
Datum rb_rank(PG_FUNCTION_ARGS);

Datum
rb_rank(PG_FUNCTION_ARGS) {
    bytea *data = PG_GETARG_BYTEA_P(0);
    uint32 value = PG_GETARG_UINT32(1);
	roaring_bitmap_t *r1;
    uint64 rank;

	r1 = roaring_bitmap_portable_deserialize_safe(VARDATA(data),
	                                              VARSIZE(data) - VARHDRSZ);
    if (!r1)
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));

	rank = roaring_bitmap_rank(r1, value);
	roaring_bitmap_free(r1);

    PG_RETURN_INT64((int64)rank);
}

//bitmap index
PG_FUNCTION_INFO_V1(rb_index);
Datum rb_index(PG_FUNCTION_ARGS);

Datum
rb_index(PG_FUNCTION_ARGS) {
    bytea *data = PG_GETARG_BYTEA_P(0);
    uint32 value = PG_GETARG_UINT32(1);
	roaring_bitmap_t *r1;
    uint64 rank;
    int64 result;
	bool isexsit;

	r1 = roaring_bitmap_portable_deserialize_safe(VARDATA(data),
	                                              VARSIZE(data) - VARHDRSZ);
    if (!r1)
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bitmap format is error")));

	isexsit = roaring_bitmap_contains(r1, value);

    result = -1;
    if(isexsit)
    {
		rank = roaring_bitmap_rank(r1, value);
		roaring_bitmap_free(r1);

        result = (int64)rank - 1;
    }
	else
	{
		roaring_bitmap_free(r1);
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
