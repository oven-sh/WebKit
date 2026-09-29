/* threadcount: a DynamoRIO client that counts the application instructions each thread executes.
 * At exit it prints one line per thread (tid, name from /proc, instructions) and totals per thread group.
 * The count is the sum of the sizes (in instructions) of the basic blocks a thread entered: an instruction
 * that faults in the middle of a block is counted with its block. */
#include "dr_api.h"
#include "drmgr.h"
#include "drreg.h"
#include <string.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <sys/prctl.h>

static reg_id_t tls_seg;
static uint tls_offs;
static void *lock;
static file_t out_file = INVALID_FILE;

#define MAX_THREADS 1024
typedef struct { thread_id_t tid; char name[32]; uint64 count; } record_t;
static record_t records[MAX_THREADS];
static int num_records;
static thread_id_t main_tid;
static int tls_index;

typedef struct { uint64 *counter; char name[32]; } per_thread_t;

static void read_name(thread_id_t tid, char *name, size_t size)
{
    char path[64];
    dr_snprintf(path, sizeof(path), "/proc/self/task/%d/comm", (int)tid);
    name[0] = 0;
    file_t f = dr_open_file(path, DR_FILE_READ);
    if (f == INVALID_FILE)
        return;
    ssize_t n = dr_read_file(f, name, size - 1);
    dr_close_file(f);
    if (n <= 0) {
        name[0] = 0;
        return;
    }
    name[n] = 0;
    for (ssize_t i = 0; i < n; ++i) {
        if (name[i] == '\n')
            name[i] = 0;
    }
}

static void event_thread_init(void *drcontext)
{
    per_thread_t *data = (per_thread_t *)dr_thread_alloc(drcontext, sizeof(per_thread_t));
    memset(data, 0, sizeof(*data));
    /* This runs in the new thread, so the segment base is the one of the thread. */
    data->counter = (uint64 *)((byte *)dr_get_dr_segment_base(tls_seg) + tls_offs);
    *data->counter = 0;
    drmgr_set_tls_field(drcontext, tls_index, data);
}

static void event_thread_exit(void *drcontext)
{
    per_thread_t *data = (per_thread_t *)drmgr_get_tls_field(drcontext, tls_index);
    thread_id_t tid = dr_get_thread_id(drcontext);
    dr_mutex_lock(lock);
    if (num_records < MAX_THREADS) {
        record_t *r = &records[num_records++];
        r->tid = tid;
        r->count = *data->counter;
        if (data->name[0])
            strncpy(r->name, data->name, sizeof(r->name) - 1);
        else
            read_name(tid, r->name, sizeof(r->name));
    }
    dr_mutex_unlock(lock);
    dr_thread_free(drcontext, data, sizeof(per_thread_t));
}

static bool event_filter_syscall(void *drcontext, int sysnum)
{
    return sysnum == SYS_prctl;
}

static bool event_pre_syscall(void *drcontext, int sysnum)
{
    if (sysnum == SYS_prctl && dr_syscall_get_param(drcontext, 0) == PR_SET_NAME) {
        per_thread_t *data = (per_thread_t *)drmgr_get_tls_field(drcontext, tls_index);
        const char *name = (const char *)dr_syscall_get_param(drcontext, 1);
        size_t read = 0;
        if (data && name && dr_safe_read(name, sizeof(data->name) - 1, data->name, &read))
            data->name[sizeof(data->name) - 1] = 0;
        else if (data && name) {
            /* The string can end before the page does: read it byte by byte. */
            size_t i = 0;
            for (; i < sizeof(data->name) - 1; ++i) {
                if (!dr_safe_read(name + i, 1, data->name + i, &read) || !data->name[i])
                    break;
            }
            data->name[i] = 0;
        }
    }
    return true;
}

static dr_emit_flags_t event_bb_analysis(void *drcontext, void *tag, instrlist_t *bb, bool for_trace, bool translating, void **user_data)
{
    uint n = 0;
    for (instr_t *instr = instrlist_first_app(bb); instr != NULL; instr = instr_get_next_app(instr))
        n++;
    *user_data = (void *)(ptr_uint_t)n;
    return DR_EMIT_DEFAULT;
}

static dr_emit_flags_t event_bb_insert(void *drcontext, void *tag, instrlist_t *bb, instr_t *instr, bool for_trace, bool translating, void *user_data)
{
    if (!drmgr_is_first_instr(drcontext, instr))
        return DR_EMIT_DEFAULT;
    uint n = (uint)(ptr_uint_t)user_data;
    if (!n)
        return DR_EMIT_DEFAULT;
    if (drreg_reserve_aflags(drcontext, bb, instr) != DRREG_SUCCESS)
        DR_ASSERT(false);
    instrlist_meta_preinsert(bb, instr,
        INSTR_CREATE_add(drcontext,
            opnd_create_far_base_disp(tls_seg, DR_REG_NULL, DR_REG_NULL, 0, tls_offs, OPSZ_8),
            OPND_CREATE_INT32((int)n)));
    if (drreg_unreserve_aflags(drcontext, bb, instr) != DRREG_SUCCESS)
        DR_ASSERT(false);
    return DR_EMIT_DEFAULT;
}

static bool has(const char *name, const char *part) { return strstr(name, part) != NULL; }

static void event_exit(void)
{
    uint64 main_count = 0, jit = 0, gc = 0, other = 0, total = 0;
    char line[256];
    char buffer[16384];
    size_t used = 0;
    for (int i = 0; i < num_records; ++i) {
        record_t *r = &records[i];
        total += r->count;
        if (r->tid == main_tid)
            main_count += r->count;
        else if (has(r->name, "JIT") || has(r->name, "Worklist") || has(r->name, "DFG") || has(r->name, "FTL") || has(r->name, "Baseline"))
            jit += r->count;
        else if (has(r->name, "Heap") || has(r->name, "GC") || has(r->name, "Collector") || has(r->name, "Marker"))
            gc += r->count;
        else
            other += r->count;
        int n = dr_snprintf(line, sizeof(line), "  thread tid=%d name=%s instructions=%llu\n", (int)r->tid, r->name, (unsigned long long)r->count);
        if (n > 0 && used + (size_t)n < sizeof(buffer)) {
            memcpy(buffer + used, line, (size_t)n);
            used += (size_t)n;
        }
    }
    int n = dr_snprintf(line, sizeof(line), "threadcount: main=%llu jit=%llu gc=%llu other=%llu total=%llu threads=%d\n",
        (unsigned long long)main_count, (unsigned long long)jit, (unsigned long long)gc, (unsigned long long)other, (unsigned long long)total, num_records);
    file_t f = out_file != INVALID_FILE ? out_file : STDERR;
    if (n > 0)
        dr_write_file(f, line, (size_t)n);
    dr_write_file(f, buffer, used);
    if (out_file != INVALID_FILE)
        dr_close_file(out_file);
    drmgr_unregister_thread_init_event(event_thread_init);
    drmgr_unregister_thread_exit_event(event_thread_exit);
    drmgr_unregister_tls_field(tls_index);
    dr_raw_tls_cfree(tls_offs, 1);
    dr_mutex_destroy(lock);
    drreg_exit();
    drmgr_exit();
}

DR_EXPORT void dr_client_main(client_id_t id, int argc, const char *argv[])
{
    dr_set_client_name("threadcount", "");
    drreg_options_t ops = { sizeof(ops), 1, false };
    if (!drmgr_init() || drreg_init(&ops) != DRREG_SUCCESS)
        DR_ASSERT(false);
    if (argc > 1)
        out_file = dr_open_file(argv[1], DR_FILE_WRITE_APPEND);
    lock = dr_mutex_create();
    main_tid = dr_get_thread_id(dr_get_current_drcontext());
    if (!dr_raw_tls_calloc(&tls_seg, &tls_offs, 1, 0))
        DR_ASSERT(false);
    tls_index = drmgr_register_tls_field();
    DR_ASSERT(tls_index != -1);
    drmgr_register_exit_event(event_exit);
    drmgr_register_filter_syscall_event(event_filter_syscall);
    drmgr_register_pre_syscall_event(event_pre_syscall);
    drmgr_register_thread_init_event(event_thread_init);
    drmgr_register_thread_exit_event(event_thread_exit);
    if (!drmgr_register_bb_instrumentation_event(event_bb_analysis, event_bb_insert, NULL))
        DR_ASSERT(false);
}
