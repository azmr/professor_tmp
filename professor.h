#ifndef PROFESSOR_H
// professor.h - a single file profiling library

// Most common
// - trigger gets hit multiple times, in uncertain hierarchy
// - init each var

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <assert.h>

#if WIN32
// NOTE: this takes a surprisingly long time to load and I only need this one definition
// if this can't be found in linking, your platform is probably not supported, sorry!
// This may be a mistake though, let me know if you think so.
// #include <intrin.h> // __rdtsc()
uint64_t __rdtsc(void);
#endif

// TODO: for DLLs prof_set_global_state
// TODO: atomics
// TODO: multiple threads
// TODO: make skippable without having to find closing tag
// TODO: combine hashmap array with normal dynamic array (hash -> index)

// NOTE: if not defined, these are not atomic!
#ifndef prof_atomic_add
#define prof_atomic_add(a, b) (*(a) += (b))
#endif

#ifndef prof_atomic_exchange
static inline uint64_t
prof_atomic_exchange(uint64_t *a, uint64_t b)
{
    uint64_t result = *a;
    *a = b;
    return result;
}
#endif

typedef uint32_t ProfIdx;

// TODO: rolling buffer of multiple frames
// TODO: add __func__?
// NOTE: these are really source locations
typedef struct ProfRecord {
	char const *name;
	char const *filename;

	uint32_t line_num;
	// record type? range/marker/thread/function/...
} ProfRecord;

// NOTE: this is really time...
typedef struct ProfRecordSmpl {
    ProfIdx  record_i;
    ProfIdx  parent_i; // if parent_i == record_smpl_i, is root
    uint64_t cycles_start;
    uint64_t cycles_end; // 0xFFFFFFF... if not finished
    // thread id/proc id?
} ProfRecordSmpl;


typedef struct ProfPtr {
    char const *name;
    char const *args;
    uintptr_t   addr;
} ProfPtr;

typedef enum ProfPtrAction {
    PROF_PTR_alloc,
    PROF_PTR_realloc,
    PROF_PTR_free,
} ProfPtrAction;

// TODO: could do some sort of linked list to connect reallocs?
typedef struct ProfPtrSmpl { // key'd by addr
    ProfIdx   record_i;
    uintptr_t addr;
    uintptr_t addr_p; // if freed / realloc'd
    uint64_t  cycles;
    size_t    size;
} ProfPtrSmpl;

// TODO: non variable length
static inline size_t
prof_fnv1a_record(ProfRecord data)
{
    unsigned char const *bytes = (unsigned char const *)&data;
    size_t               hash  = 0xcbf29ce484222325;
    for (size_t i = 0; i < sizeof(data); ++i)
    {
        unsigned char b = bytes[i];
        hash ^= b;
        hash *= 0x100000001b3;
    }
    return hash;
}

static inline int
prof_record_eq(ProfRecord a, ProfRecord b)
{
    return (a.name     == b.name &&
            a.filename == b.filename &&
            a.line_num == b.line_num);
}

// TODO
#define MAP_INVALID_VAL (~(ProfIdx) 0)
#define MAP_HASH_KEY(key) prof_fnv1a_record(key)
#define MAP_KEY_EQ(a, b) prof_record_eq(a, b)
#define MAP_TYPES (ProfRecordMap, prof_record_map, ProfRecord, ProfIdx)
#include "hash.h"

typedef struct Prof {
    ProfRecord *records; // dynamic array
    ProfIdx     records_n, records_m;
    // could calculate this from the record tree...
	/* uint64_t *hits_n__cycles_n; // Parallel with records, Top half hits_n, bottom half cycles_n */
    ProfRecordMap dyn_records_i_map[1]; // maps ProfRecord to index in records (or ~0 if not found)

    ProfRecordSmpl *record_smpl_tree; // dynamic
    ProfIdx         record_smpl_tree_n, record_smpl_tree_m;
    ProfIdx         open_record_smpl_tree_i; // the deepest record that is still open (check if this record is closed to see if all are closed)

    ProfPtrSmpl *ptr_smpls;
    ProfIdx      ptr_smpls_n, ptr_smpls_m;

    double freq;

    // NOTE: this is needed so that different allocators aren't used across dll boundaries
    void *(*reallocate)(void *allocator, void *ptr, size_t size);
    void *allocator;
} Prof;

// TODO: preprocessor gate
#include <stdlib.h>
static void *
prof_realloc(void *allocator, void *ptr, size_t size)
{
    (void)allocator;
    return realloc(ptr, size);
}

static inline void *
prof_grow(Prof *prof, void *ptr, ProfIdx *max, size_t elem_size)
{
    // TODO: make threadsafe

    if (! prof->reallocate)
    {   prof->reallocate = prof_realloc;   }

    *max = (*max
            ? *max * 2
            : 64);
    size_t size = *max * elem_size;
    void *result = prof->reallocate(prof->allocator, ptr, size);
    return result;
}

static inline ProfIdx
prof_top_record_i(Prof *prof)
{
    // TODO: thread id...
    ProfIdx result = ((prof->record_smpl_tree_n &&
                       ~prof->open_record_smpl_tree_i)
                      ? prof->record_smpl_tree[prof->open_record_smpl_tree_i].record_i
                      : ~(ProfIdx) 0);
    return result;
}

static inline ProfIdx
prof_new_record(Prof *prof, char const *name, char const *filename, uint32_t line_num)
{
    if (prof->records_n == prof->records_m)
    {   prof->records = (ProfRecord *)prof_grow(prof, prof->records, &prof->records_m, sizeof(*prof->records));   }

    ProfRecord record = {0}; {
        record.name     = name;
        record.filename = filename;
        record.line_num = line_num;
    }
    ProfIdx result        = prof->records_n++;
    prof->records[result] = record;

    return result;
}

// checks if there's an existing record. If so, returns that index, otherwise creates a new one and returns the new index
static inline ProfIdx
prof_add_dyn_record(Prof *prof, char const *name, char const *filename, uint32_t line_num)
{
    ProfRecord record = {0}; {
        record.name     = name;
        record.filename = filename;
        record.line_num = line_num;
    }

    ProfIdx result = prof_record_map_get(prof->dyn_records_i_map, record);
    if (! ~ result)
    {
        result = prof_new_record(prof, name, filename, line_num);
        prof_record_map_insert(prof->dyn_records_i_map, record, result);
    }

    return result;
}

static inline void
prof_start_(Prof *prof, ProfIdx record_i)
{
    uint64_t cycles_start = __rdtsc();
    ProfIdx  parent_i = (~ prof->open_record_smpl_tree_i
                         ? prof->open_record_smpl_tree_i
                         : prof->record_smpl_tree_n);

    if (prof->record_smpl_tree_n == prof->record_smpl_tree_m)
    {   prof->record_smpl_tree = (ProfRecordSmpl *)prof_grow(prof, prof->record_smpl_tree, &prof->record_smpl_tree_m, sizeof(*prof->record_smpl_tree));   }

    ProfRecordSmpl record_smpl; {
        record_smpl.record_i     = record_i;
        record_smpl.parent_i     = parent_i;
        record_smpl.cycles_start = cycles_start;
        record_smpl.cycles_end   = ~(uint64_t) 0;
    };

    prof->open_record_smpl_tree_i = prof->record_smpl_tree_n++;
    prof->record_smpl_tree[prof->open_record_smpl_tree_i] = record_smpl;
}

static inline void
prof_mark_(Prof *prof, ProfIdx record_i)
{
    uint64_t cycles = __rdtsc();
    ProfIdx  parent_i = prof->open_record_smpl_tree_i;

    if (prof->record_smpl_tree_n == prof->record_smpl_tree_m)
    {   prof->record_smpl_tree = (ProfRecordSmpl *)prof_grow(prof, prof->record_smpl_tree, &prof->record_smpl_tree_m, sizeof(*prof->record_smpl_tree));   }

    ProfRecordSmpl record_smpl; {
        record_smpl.record_i     = record_i;
        record_smpl.parent_i     = parent_i;
        record_smpl.cycles_start = cycles;
        record_smpl.cycles_end   = cycles;
    };

    prof->record_smpl_tree[prof->record_smpl_tree_n++] = record_smpl;
}


static inline void
prof_ptr_realloc_(Prof *prof, ProfIdx record_i, void *addr, void *addr_p, size_t size)
{
    uint64_t cycles = __rdtsc();

    if (prof->ptr_smpls_n == prof->ptr_smpls_m)
    {   prof->ptr_smpls = (ProfPtrSmpl *)prof_grow(prof, prof->ptr_smpls, &prof->ptr_smpls_m, sizeof(*prof->ptr_smpls));   }

    ProfPtrSmpl ptr_smpl = {0}; {
        ptr_smpl.record_i = record_i;
        ptr_smpl.addr     = (uintptr_t)addr;
        ptr_smpl.addr_p   = (uintptr_t)addr_p; // if realloc'd
        ptr_smpl.cycles   = cycles;
        ptr_smpl.size     = size;   // if 0, was freed
    }

    prof->ptr_smpls[prof->ptr_smpls_n++] = ptr_smpl;
}

#ifdef  PROF_PRINT_SCOPE
static inline void
prof_print_scope(Prof const *prof)
{
    ProfIdx smpl_i = prof->open_record_smpl_tree_i;
    if (~smpl_i)
    {
        ProfRecordSmpl const *smpl_tree = prof->record_smpl_tree;
        ProfRecord            record    = prof->records[smpl_tree[smpl_i].record_i];

        while (smpl_i != smpl_tree[smpl_i].parent_i)
        {
            fputs("  ", stdout);
            smpl_i = smpl_tree[smpl_i].parent_i;
        }

        printf("%s (%s : %u)\n", record.name, record.filename, record.line_num);
    }
}

#else //PROF_PRINT_SCOPE
#define prof_print_scope(...)
#endif//PROF_PRINT_SCOPE

#if PROFESSOR_DISABLE
# define prof_start(prof, name)
# define prof_mark(prof, name)
# define prof_ptr_realloc(prof, name, addr, addr_p)
# define prof_ptr_alloc(prof, name, ptr, size)
# define prof_ptr_free( prof, name, ptr)
# define prof_scope(prof, name)
# define prof_scope_n(prof, name, n)

# define prof_start_fn(prof)
# define prof_end_n_fn(prof, n)
# define prof_end_fn(prof)
# define prof_end_unchecked(...)
# define prof_end_n(...)
# define prof_end(...)

#else // PROFESSOR_DISABLE
# define PROF_CAT1(a,b) a ## b
# define PROF_CAT2(a,b) PROF_CAT1(a,b)
# define PROF_CAT(a,b)  PROF_CAT2(a,b)
# define prof_static_local_record_i_ PROF_CAT(prof_static_local_record_i_, __LINE__)
# define prof_scope_once PROF_CAT(prof_scope_once, __LINE__)

# define PROF_NEW_RECORD(prof, name) \
    static ProfIdx prof_static_local_record_i_ = ~(ProfIdx) 0; \
    if (! ~prof_static_local_record_i_) /* TODO: atomic */ \
    {   prof_static_local_record_i_ = prof_new_record(prof, name, __FILE__, __LINE__);   } \

# define prof_start(prof, name) \
    do { \
        PROF_NEW_RECORD(prof, name) \
        prof_start_(prof, prof_static_local_record_i_); \
        prof_print_scope(prof); \
        /* __itt_task_begin(0, __itt_null, __itt_null, __itt_string_handle_createA(name));\ */ \
    } while (0)

# define prof_mark(prof, name) \
    do { \
        PROF_NEW_RECORD(prof, name) \
        prof_mark_(prof, prof_static_local_record_i_); \
        prof_print_scope(prof); \
    } while (0)

# define prof_ptr_realloc(prof, name, addr, addr_p) \
    do { \
        PROF_NEW_RECORD(prof, name) \
        prof_ptr_realloc_(prof, prof_static_local_record_i_, addr, addr_p, size); \
    } while (0)

# define prof_ptr_alloc(prof, name, ptr, size) prof_ptr_realloc(prof, name, addr, 0, size)
# define prof_ptr_free( prof, name, ptr)       prof_ptr_realloc(prof, name, addr, 0, 0)


// NOTE: can't nest without braces
# define prof_scope(prof, name) prof_scope_n(prof, name, 1)
# define prof_scope_n(prof, name, n) prof_start(prof, name); \
    for (int prof_scope_once = 0; prof_scope_once++ == 0; prof_end_n_unchecked(prof, n))
# define prof_exit_scope continue

// returns the index of the record referenced, so you can double check this is correct
static inline ProfIdx
prof_end_n_unchecked(Prof *prof, uint32_t hits_n)
{   (void)hits_n; // TODO: incorporate this
    /* __itt_task_end(0); */
    assert(prof->record_smpl_tree   &&
           prof->record_smpl_tree_n &&
           "no record samples taken at all - nothing to close");
    assert(~prof->open_record_smpl_tree_i &&
           "no open prof records - you've already closed them all. Mismatched start and end records?");

    ProfRecordSmpl *record_smpl      = &prof->record_smpl_tree[prof->open_record_smpl_tree_i];
    uint64_t        cycles_end       = __rdtsc();
    uint64_t        cycles_n         = cycles_end - record_smpl->cycles_start;
    /* uint64_t        hits_n__cycles_n = (uint64_t)cycles_n | ((uint64_t)hits_n << 32); */

    record_smpl->cycles_end = cycles_end;
    /* prof_atomic_add(&prof->records[record_smpl->record_i].hits_n__cycles_n, hits_n__cycles_n); // TODO: this could be done after the fact */

    int is_tree_root = prof->open_record_smpl_tree_i == record_smpl->parent_i;
    prof->open_record_smpl_tree_i = (! is_tree_root
                                     ? record_smpl->parent_i
                                     : ~(ProfIdx) 0);

    return record_smpl->record_i;
}

static inline ProfIdx
prof_end_n(Prof *prof, ProfIdx expected_record_i, uint32_t hits_n)
{
    ProfIdx actual_record_i = prof_end_n_unchecked(prof, hits_n);
    assert((!~expected_record_i || // if you don't want to check here
            actual_record_i == expected_record_i) && "prof start and end don't seem to match");
    return actual_record_i;
}

static inline ProfIdx
prof_end(Prof *prof, ProfIdx expected_record_i)
{   return prof_end_n(prof, expected_record_i, 1);   }

static inline ProfIdx
prof_end_unchecked(Prof *prof)
{   return prof_end_n_unchecked(prof, 1);   }

# define prof_start_fn(prof)    prof_start(prof, __func__); ProfIdx prof_fn_local_record_i = prof_top_record_i(prof)
# define prof_end_n_fn(prof, n) prof_end_n(prof, prof_fn_local_record_i, n)
# define prof_end_fn(prof)      prof_end_n_fn(prof, 1)
#endif // PROFESSOR_DISABLE

#if 1 // OUTPUT

/* static inline void */
/* prof_record_read_clear(Prof *prof, ProfIdx record_i, uint32_t *hits_n, uint32_t *cycles_n) */
/* { */
/*     uint64_t hits_n__cycles_n = prof->records[record_i].hits_n__cycles_n; */
/*     if (hits_n)   { *hits_n   = (uint32_t)(hits_n__cycles_n >> 32); } */
/*     if (cycles_n) { *cycles_n = (uint32_t)(hits_n__cycles_n & 0xFFFFFFFF); } */
/* } */

static void
prof_dump_still_open(FILE *out, Prof const *prof)
{
    for (ProfIdx top_i = prof->open_record_smpl_tree_i;
         ~top_i;
         top_i = prof->record_smpl_tree[top_i].parent_i)
    {
        ProfRecordSmpl smpl = prof->record_smpl_tree[top_i];
        ProfRecord     record = prof->records[smpl.record_i];
        fprintf(out, "sample: %d, record[%d]: %s (%s[%u])\n",
                top_i, smpl.record_i,
                record.name,
                record.filename,
                record.line_num);

        if (top_i == prof->record_smpl_tree[top_i].parent_i)
        {   break;   }
    }
    fputc('\n', out);
}

// *out can be NULL the first time to init, otherwise ensure there's a '[' at the beginning of the file
static void
prof_dump_timings_file(FILE **out, char const *filename, Prof *prof)
{
    double ms = (prof->freq != 0.0
                 ? prof->freq / 1000.0
                 : 1.0);
    ProfRecord *records   = prof->records;

    if (! *out)
    {
        *out = fopen(filename, "w");
        assert(*out);
        fputs("[\n", *out);
    }
    else
    {   fputs(",\n\n", *out);   }

    ProfRecordSmpl *record_smpl_tree   = prof->record_smpl_tree;
    size_t          record_smpl_tree_n = prof->record_smpl_tree_n;
    for (size_t record_smpl_tree_i = 0; record_smpl_tree_i < record_smpl_tree_n; ++record_smpl_tree_i)
    {
        // TODO: units
        ProfRecordSmpl record_smpl = record_smpl_tree[record_smpl_tree_i];
        ProfRecord     record      = records[record_smpl.record_i];
        if (record_smpl_tree_i > 0)
        {   fprintf(*out, ",\n");   }

        // TODO: should these just be in separate arrays?
        if (record_smpl.cycles_start != record_smpl.cycles_end)
        { // normal record
            fprintf(*out, "    {"
                    "\"name\":\"%s\", "
                    "\"ph\":\"X\", "
                    "\"ts\": %lf, "
                    "\"dur\": %lf, "
                    "\"pid\": 0, "
                    "\"tid\": 0"
                    "}",
                    /* record.filename, */
                    record.name,
                    record_smpl.cycles_start / ms,
                    (record_smpl.cycles_end - record_smpl.cycles_start) / ms
            );
        }

        else
        { // mark
            fprintf(*out, "    {"
                    "\"name\":\"%s\", "
                    "\"ph\":\"i\", "
                    "\"ts\": %lf, "
                    "\"pid\": 0, "
                    "\"tid\": 0"
                    "}",
                    /* record.filename, */
                    record.name,
                    record_smpl.cycles_start / ms
            );
        }
    }

#if 0 // MEMORY sampling
    fprintf(out, ",\n\n");

    ProfPtrSmpl *ptr_smpls   = prof->ptr_smpls;
    size_t       ptr_smpls_n = prof->ptr_smpls_n;

    ProfPtrSmpl **opens = 0;
    ProfIdx open_n = 0, open_m = 0;

    { // introduce all data series
        fprintf(out, "    {"
                "\"name\":\"memory\", "
                "\"ph\":\"C\", "
                "\"ts\": %lf, "
                "\"args\": {"
                , ptr_smpls[0].cycles / ms
        );

        for (size_t ptr_smpls_i = 0; ptr_smpls_i < ptr_smpls_n; ++ptr_smpls_i)
        {
            if (ptr_smpls_i > 0)
            {   fputs(",", out);   }
            fprintf(out, "\"0x%08llx\": 0", ptr_smpls[ptr_smpls_i].addr);
        }

        fprintf(out,
                "}, "
                "\"pid\": 0, "
                "\"tid\": 0"
                "}");

        fputs(",\n\n", out);
    }

    for (size_t ptr_smpls_i = 0; 0 && ptr_smpls_i < ptr_smpls_n; ++ptr_smpls_i)
    { // memory count
        ProfPtrSmpl ptr_smpl = ptr_smpls[ptr_smpls_i];
        ProfRecord  record   = records[ptr_smpl.record_i];

        { // update open array
            if (ptr_smpl.size > 0)
            { // some mem was (re)alloc'd
                if (ptr_smpl.addr_p) // realloc
                { // replace open ptr with this one
                    ProfIdx already_open_i = ~(ProfIdx)0;
                    for (ProfIdx open_i = 0; open_i < open_n; ++open_i)
                    { // check if this pointer has been alloc'd but not freed
                        ProfPtrSmpl *open = opens[open_i];

                        assert((ptr_smpl.addr == ptr_smpl.addr_p || // realloc'ing in place
                                ptr_smpl.addr != open->addr)
                               && "the same address cannot be opened multiple times");

                        if (open->addr == ptr_smpl.addr_p)
                        {   already_open_i = open_i; break;   }
                    }
                    assert(~already_open_i && "there must be a pointer to replace");
                    opens[already_open_i] = &ptr_smpls[ptr_smpls_i];
                }

                else // straight alloc
                { // append current smpl to open list
                    if (open_n == open_m)
                    {   opens = (ProfPtrSmpl **)prof_grow(prof, opens, &open_m, sizeof(*opens));   }
                    ptr_smpls = prof->ptr_smpls; // the allocation from prof_grow may have moved the array

                    opens[open_n++] = &ptr_smpls[ptr_smpls_i];
                }
            }

            else // free
            { // endswap remove open smpl
                ProfIdx already_open_i = ~(ProfIdx)0;
                for (ProfIdx open_i = 0; open_i < open_n; ++open_i)
                { // check if this pointer has been alloc'd but not freed
                    ProfPtrSmpl *open = opens[open_i];
                    if (open->addr == ptr_smpl.addr)
                    {   already_open_i = open_i; break;   }
                }
                assert(~already_open_i && "there must be a pointer to remove");
                opens[already_open_i] = opens[--open_n];
            }
        }

        if (ptr_smpls_i > 0)
        {   fputs(",\n", out);   }

        fprintf(out, "    {"
                "\"name\":\"memory\", "
                "\"ph\":\"C\", "
                "\"ts\": %lf, "
                "\"args\": {"
                , ptr_smpl.cycles / ms
        );

        for (size_t open_i = 0; open_i < open_n; ++open_i)
        {
            ProfPtrSmpl open = *opens[open_i];

            if (open_i > 0)
            {   fputs(",", out);   }

            fprintf(out, "\"0x%08llx\": %llu", open.addr, open.size);
        }

        fprintf(out,
                "}, "
                "\"pid\": 0, "
                "\"tid\": 0"
                "}");
    }

    { // continue still open memory to last profiling time
        uint64_t final_cycles = 0;
        for (ProfIdx smpl_i = 0; smpl_i < record_smpl_tree_n; ++ smpl_i)
        { // find last profiling time
            uint64_t smpl_cycles = record_smpl_tree[smpl_i].cycles_end;
            if (smpl_cycles > final_cycles)
            {   final_cycles = smpl_cycles;   } // NOTE: there's a slight discrepancy here, I think due to rounding from "dur" = begin + (end-begin)
        }

        fprintf(out, ",\n    {"
                "\"name\":\"memory\", "
                "\"ph\":\"C\", "
                "\"ts\": %lf, "
                "\"args\": {"
                , final_cycles / ms
        );

        for (ProfIdx open_i = 0; open_i < open_n; ++open_i)
        {
            ProfPtrSmpl open = *opens[open_i];

            if (open_i > 0)
            {   fputs(",", out);   }

            fprintf(out, "\"0x%08llx\": %llu", open.addr, open.size);
        }

        fprintf(out,
                "}, "
                "\"pid\": 0, "
                "\"tid\": 0"
                "}");
    }
#endif

    fflush(*out);

    prof->record_smpl_tree_n = 0;
}
#endif // OUTPUT

#if 1 // INVARIANTS

#include <string.h>

static inline int
prof_invar_unique_records(Prof *prof)
{
    int result = 1;
    ProfRecord *records = prof->records;

    for (ProfIdx record_i = 0; record_i < prof->records_n; ++record_i)
    {
        ProfRecord record = records[record_i];

        for (ProfIdx record_j = record_i + 1; record_j < prof->records_n; ++record_j)
        {
            ProfRecord test_record = records[record_j];

            if (record.line_num == test_record.line_num &&
                ! strcmp(record.name, test_record.name) &&
                ! strcmp(record.filename, test_record.filename))
            {   result = 0; break;   }
        }
    }

    return result;
}

#endif // INVARIANTS

#define PROFESSOR_H
#endif//PROFESSOR_H
