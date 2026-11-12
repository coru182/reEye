/* reEye: a checking front for the title heap, for finding what damages it. Built in when build-ps5.sh runs with
 * HEAPCHECK=1 (the default while the first-frame crash is open); with HEAPCHECK=0 this file is empty and the
 * payload SDK's own wraps (ps5platform's heap_wrap.c) serve the program.
 *
 * Every allocation the program makes reaches these functions through the link's --wrap=malloc (and the rest of the
 * family, the same list as heap_wrap.c), and takes its memory from the same title heap (ps5platform/heap.h),
 * with more around it:
 *
 *   [header: size, where it was allocated (a call stack), where it was freed] [16 guard bytes] [the block]
 *   [256 guard bytes]
 *
 * - The guard bytes on each side hold a fixed pattern. A program that writes past either end of its block changes
 *   them, instead of the heap's own records next to the block, where the damage shows up much later as an abort
 *   inside malloc with no hint of who did it (the crash this was built for).
 * - A freed block is filled with 0xDD and kept back (up to 64 MB of them) before it goes back to the heap, so a
 *   write through a pointer kept after free changes the fill.
 * - The guards are checked when a block is freed or resized, the fill when a kept-back block goes back, and every
 *   live and kept-back block every 2 seconds by a thread of its own (reeye_heapcheck_start), and once more when
 *   the program aborts (reeye_heapcheck_scan, from ps5/app_ps5.cpp's abort handler).
 *
 * The first damage found is written to /data/reeye/heapcheck.txt, without allocating: what was damaged, the block's
 * size, the call stack that allocated it (and freed it, for a write after free), the bytes found, and the call
 * stack that found it; then the program aborts. tools/symbolize-crash.sh turns the stacks into function names. */
#if defined(REEYE_HEAPCHECK) && REEYE_HEAPCHECK

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <unwind.h>

#ifndef HC_REPORT_PATH
#define HC_REPORT_PATH "/data/reeye/heapcheck.txt"
#endif

/* ps5platform/heap.h and heap.c */
void *ps5_heap_malloc(size_t bytes);
void *ps5_heap_memalign(size_t alignment, size_t bytes);
void ps5_heap_free(void *pointer);
bool ps5_heap_owns(const void *pointer);
void ps5p_heap_count_fallback(void);

/* libc's allocator, through the link's --wrap */
void *__real_malloc(size_t bytes);
void *__real_calloc(size_t count, size_t bytes);
void *__real_realloc(void *pointer, size_t bytes);
void __real_free(void *pointer);
int __real_posix_memalign(void **out, size_t alignment, size_t bytes);
__attribute__((weak)) size_t __real_malloc_usable_size(const void *pointer);

extern int main(int argc, char **argv);

#define HC_ALLOC_FRAMES 10
#define HC_FREE_FRAMES 6
#define HC_FRONT 16
#define HC_TAIL 256
#define HC_SHARDS 64
#define HC_QUARANTINE_ENTRIES 65536
#define HC_QUARANTINE_BYTES ((size_t)64 << 20)
/* larger blocks go back to the heap at once (checked, not kept) */
#define HC_QUARANTINE_LARGEST ((size_t)16 << 20)
#define HC_ALIGN 32

#define HC_LIVE UINT64_C(0x45564c4945455945)  /* "EYEEILVE" */
#define HC_FREED UINT64_C(0x44455246ee455945) /* "EYE.FRED" */
#define HC_FRONT_BYTE 0xa5
#define HC_TAIL_BYTE 0xfb
#define HC_FREED_BYTE 0xdd

struct hc_block {
   uint64_t magic;
   uint64_t size;
   void *base; /* what the title heap returned */
   struct hc_block *prev, *next;
   uint64_t serial;
   uintptr_t alloc[HC_ALLOC_FRAMES];
   uintptr_t freed[HC_FREE_FRAMES];
   uint8_t pad[64];
   uint8_t front[HC_FRONT];
};
_Static_assert(sizeof(struct hc_block) == 256, "the header keeps the block 32-byte aligned");
#define HC_HEADER sizeof(struct hc_block)

struct hc_shard {
   pthread_mutex_t lock;
   struct hc_block *head;
};
static struct hc_shard shards[HC_SHARDS];
static pthread_once_t shards_once = PTHREAD_ONCE_INIT;

static pthread_mutex_t quarantine_lock = PTHREAD_MUTEX_INITIALIZER;
static struct hc_block *quarantine[HC_QUARANTINE_ENTRIES];
static unsigned quarantine_head, quarantine_count;
static size_t quarantine_bytes;

static uint64_t next_serial;
static uint64_t live_blocks, passes, last_pass_blocks;
static int reported;
/* Set on a thread while it records a call stack, so an allocation the unwinder might make records none. A pthread
 * key and not _Thread_local: the PS5 build's thread-local variables are emulated (-femulated-tls), and a thread's
 * first use of one allocates it with malloc, which would come back here forever (0.0.1's stack overflow before
 * main). The keys live in the system library, which never calls this heap. */
static pthread_key_t capturing_key;
/* Call stacks are recorded from reeye_heapcheck_start (in main) on: before main the unwinder may not be ready, and
 * blocks allocated then are checked all the same, without a stack */
static int stacks_on;

static void
shards_init(void)
{
   for (unsigned i = 0; i < HC_SHARDS; i++)
      pthread_mutex_init(&shards[i].lock, NULL);
   pthread_key_create(&capturing_key, NULL);
}

static struct hc_shard *
shard_of(const struct hc_block *b)
{
   return &shards[((uintptr_t)b >> 8) % HC_SHARDS];
}

static struct hc_block *
header_of(const void *user)
{
   return (struct hc_block *)((uintptr_t)user - HC_HEADER);
}

static uint8_t *
user_of(const struct hc_block *b)
{
   return (uint8_t *)b + HC_HEADER;
}

/* ------------------------------------------------------------- call stacks */

struct hc_frames {
   uintptr_t *pc;
   int max, count, skip;
};

static _Unwind_Reason_Code
hc_collect(struct _Unwind_Context *context, void *arg)
{
   struct hc_frames *f = arg;
   if (f->skip > 0) {
      f->skip--;
      return _URC_NO_REASON;
   }
   if (f->count >= f->max)
      return _URC_END_OF_STACK;
   f->pc[f->count++] = (uintptr_t)_Unwind_GetIP(context);
   return _URC_NO_REASON;
}

/* The calling thread's stack, without this function's own frame and the `skip` frames above it */
__attribute__((noinline)) static void
hc_capture(uintptr_t *pc, int max, int skip)
{
   memset(pc, 0, (size_t)max * sizeof(*pc));
   if (!__atomic_load_n(&stacks_on, __ATOMIC_ACQUIRE))
      return;
   pthread_once(&shards_once, shards_init);
   if (pthread_getspecific(capturing_key))
      return;
   pthread_setspecific(capturing_key, (void *)1);
   struct hc_frames f = {.pc = pc, .max = max, .count = 0, .skip = 1 + skip};
   _Unwind_Backtrace(hc_collect, &f);
   pthread_setspecific(capturing_key, NULL);
}

/* ------------------------------------------------------------------ checks */

/* The first byte from p on (n bytes) that is not `value`, or NULL */
static const uint8_t *
hc_first_not(const uint8_t *p, size_t n, uint8_t value)
{
   uint64_t word;
   memset(&word, value, sizeof(word));
   size_t i = 0;
   while (i < n && ((uintptr_t)(p + i) & 7))
      if (p[i++] != value)
         return p + i - 1;
   for (; i + 8 <= n; i += 8) {
      uint64_t w;
      memcpy(&w, p + i, 8);
      if (w != word)
         break;
   }
   for (; i < n; i++)
      if (p[i] != value)
         return p + i;
   return NULL;
}

static bool
hc_plausible(const struct hc_block *b)
{
   return b && ((uintptr_t)b & (HC_ALIGN - 1)) == 0 && ps5_heap_owns(b) && ps5_heap_owns((const uint8_t *)b + HC_HEADER - 1);
}

/* ------------------------------------------------------------------ report */

static int report_fd = -1;

static void
out(const char *format, ...) __attribute__((format(printf, 1, 2)));

static void
out(const char *format, ...)
{
   if (report_fd < 0)
      return;
   char line[256];
   va_list args;
   va_start(args, format);
   int n = vsnprintf(line, sizeof(line), format, args);
   va_end(args);
   if (n > 0)
      write(report_fd, line, (size_t)(n < (int)sizeof(line) ? n : (int)sizeof(line) - 1));
}

static void
out_frames(const char *title, const uintptr_t *pc, int count)
{
   if (!pc[0])
      return;
   out("%s\n", title);
   for (int i = 0; i < count && pc[i]; i++)
      out("  #%d 0x%llx\n", i, (unsigned long long)pc[i]);
}

static void
out_bytes(const char *title, const uint8_t *p, size_t n, const uint8_t *mark)
{
   out("%s\n", title);
   for (size_t row = 0; row < n; row += 16) {
      char line[96];
      int at = snprintf(line, sizeof(line), "  %p ", (const void *)(p + row));
      for (size_t i = row; i < row + 16 && i < n && at < (int)sizeof(line) - 4; i++)
         at += snprintf(line + at, sizeof(line) - (size_t)at, p + i == mark ? "[%02x]" : " %02x ", p[i]);
      out("%s\n", line);
   }
}

static void
out_block(const char *title, const struct hc_block *b)
{
   out("%s %p: %llu bytes at %p, allocation #%llu\n", title, (const void *)b, (unsigned long long)b->size,
       (const void *)user_of(b), (unsigned long long)b->serial);
   out_frames("allocated by:", b->alloc, HC_ALLOC_FRAMES);
   if (b->magic == HC_FREED)
      out_frames("freed by:", b->freed, HC_FREE_FRAMES);
}

/* The live block closest below `address` in memory: when a block's start is damaged, the block below it wrote past
 * its end. Walked without the locks (the report is the last thing the program does). */
static const struct hc_block *
hc_block_below(uintptr_t address)
{
   const struct hc_block *best = NULL;
   for (unsigned s = 0; s < HC_SHARDS; s++) {
      unsigned steps = 0;
      for (const struct hc_block *b = shards[s].head; b && steps < 50000000; b = b->next, steps++) {
         if (!hc_plausible(b) || b->magic != HC_LIVE)
            break;
         if ((uintptr_t)b < address && (!best || (uintptr_t)b > (uintptr_t)best))
            best = b;
      }
   }
   return best;
}

/* Writes the report for the first damage found. `b` is the damaged block (NULL when only `pointer` is known), `at`
 * the first damaged byte. */
__attribute__((noinline)) static void
hc_report(const char *what, const struct hc_block *b, const void *pointer, const uint8_t *at, bool then_abort)
{
   if (__atomic_exchange_n(&reported, 1, __ATOMIC_ACQ_REL)) {
      if (then_abort)
         abort();
      return;
   }
   report_fd = open(HC_REPORT_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0666);
   out("heapcheck: %s\n", what);
   out("main at %p\nthread %p\n", (void *)&main, (void *)pthread_self());
   if (pointer)
      out("pointer %p\n", pointer);
   if (b && hc_plausible(b)) {
      out_block("block", b);
      if (at) {
         const uint8_t *start = at - ((uintptr_t)at & 15);
         out("first damaged byte %p, %+lld from the block's start\n", (const void *)at,
             (long long)(at - user_of(b)));
         out_bytes("bytes there (expected a5 before the block, fb after it, dd inside a freed one):", start - 16, 64,
                   at);
      }
      /* damage before the block: the one below wrote past its end */
      if (at && at < user_of(b)) {
         const struct hc_block *below = hc_block_below((uintptr_t)b);
         if (below)
            out_block("the block below it in memory (it may have written past its end)", below);
      }
   } else if (b) {
      out("block header %p is not in the heap\n", (const void *)b);
   }
   uintptr_t here[16];
   hc_capture(here, 16, 0);
   out_frames("found by:", here, 16);
   close(report_fd);
   report_fd = -1;
   if (then_abort)
      abort();
}

/* Checks a block's header and guards; reports and aborts on damage */
static void
hc_check(const struct hc_block *b, const char *during)
{
   char what[128];
   if (b->magic == HC_FREED) {
      snprintf(what, sizeof(what), "%s of a block already freed", during);
      hc_report(what, b, user_of(b), NULL, true);
   }
   if (b->magic != HC_LIVE) {
      snprintf(what, sizeof(what), "%s: the block's header is overwritten (or the pointer is not a block's start)",
               during);
      hc_report(what, b, user_of(b), (const uint8_t *)&b->magic, true);
   }
   const uint8_t *bad = hc_first_not(b->front, HC_FRONT, HC_FRONT_BYTE);
   if (bad) {
      snprintf(what, sizeof(what), "%s: written before the block's start", during);
      hc_report(what, b, user_of(b), bad, true);
   }
   bad = hc_first_not(user_of(b) + b->size, HC_TAIL, HC_TAIL_BYTE);
   if (bad) {
      snprintf(what, sizeof(what), "%s: written past the block's end", during);
      hc_report(what, b, user_of(b), bad, true);
   }
}

/* A kept-back block: its guards, and its fill */
static void
hc_check_freed(const struct hc_block *b)
{
   if (b->magic != HC_FREED)
      hc_report("a freed block's header is overwritten", b, user_of(b), (const uint8_t *)&b->magic, true);
   const uint8_t *bad = hc_first_not(b->front, HC_FRONT, HC_FRONT_BYTE);
   if (!bad)
      bad = hc_first_not(user_of(b), b->size, HC_FREED_BYTE);
   if (!bad)
      bad = hc_first_not(user_of(b) + b->size, HC_TAIL, HC_TAIL_BYTE);
   if (bad)
      hc_report("written after free", b, user_of(b), bad, true);
}

/* ---------------------------------------------------------------- the heap */

__attribute__((noinline)) static void *
hc_alloc(size_t size, size_t alignment)
{
   pthread_once(&shards_once, shards_init);
   if (alignment < HC_ALIGN)
      alignment = HC_ALIGN;
   const size_t room = (HC_HEADER + alignment - 1) & ~(alignment - 1);
   if (size > SIZE_MAX - room - HC_TAIL)
      return NULL;
   const size_t total = room + size + HC_TAIL;
   void *const base = alignment > HC_ALIGN ? ps5_heap_memalign(alignment, total) : ps5_heap_malloc(total);
   if (!base)
      return NULL;
   uint8_t *const user = (uint8_t *)base + room;
   struct hc_block *const b = header_of(user);
   b->magic = HC_LIVE;
   b->size = size;
   b->base = base;
   b->serial = __atomic_add_fetch(&next_serial, 1, __ATOMIC_RELAXED);
   hc_capture(b->alloc, HC_ALLOC_FRAMES, 2); /* from the caller of the wrap on */
   memset(b->freed, 0, sizeof(b->freed));
   memset(b->front, HC_FRONT_BYTE, HC_FRONT);
   memset(user + size, HC_TAIL_BYTE, HC_TAIL);
   struct hc_shard *const s = shard_of(b);
   pthread_mutex_lock(&s->lock);
   b->prev = NULL;
   b->next = s->head;
   if (s->head)
      s->head->prev = b;
   s->head = b;
   pthread_mutex_unlock(&s->lock);
   __atomic_add_fetch(&live_blocks, 1, __ATOMIC_RELAXED);
   return user;
}

static void
hc_release(struct hc_block *b)
{
   ps5_heap_free(b->base);
}

/* Frees a block of the title heap that hc_alloc gave out */
__attribute__((noinline)) static void
hc_free(void *pointer, const char *during)
{
   struct hc_block *const b = header_of(pointer);
   if (((uintptr_t)pointer & (HC_ALIGN - 1)) || !hc_plausible(b)) {
      char what[96];
      snprintf(what, sizeof(what), "%s of a pointer the heap never gave out", during);
      hc_report(what, NULL, pointer, NULL, true);
   }
   hc_check(b, during);
   struct hc_shard *const s = shard_of(b);
   pthread_mutex_lock(&s->lock);
   if (b->prev)
      b->prev->next = b->next;
   else
      s->head = b->next;
   if (b->next)
      b->next->prev = b->prev;
   pthread_mutex_unlock(&s->lock);
   __atomic_sub_fetch(&live_blocks, 1, __ATOMIC_RELAXED);
   b->magic = HC_FREED;
   b->prev = b->next = NULL;
   hc_capture(b->freed, HC_FREE_FRAMES, 2);
   memset(user_of(b), HC_FREED_BYTE, b->size);
   if (b->size > HC_QUARANTINE_LARGEST) {
      hc_release(b);
      return;
   }
   /* kept back; the oldest go back to the heap, checked */
   struct hc_block *evicted[8];
   unsigned n = 0;
   pthread_mutex_lock(&quarantine_lock);
   while (n < 8 && quarantine_count &&
          (quarantine_count >= HC_QUARANTINE_ENTRIES || quarantine_bytes + b->size > HC_QUARANTINE_BYTES)) {
      struct hc_block *const old = quarantine[quarantine_head];
      quarantine_head = (quarantine_head + 1) % HC_QUARANTINE_ENTRIES;
      quarantine_count--;
      quarantine_bytes -= old->size;
      evicted[n++] = old;
   }
   bool kept = quarantine_count < HC_QUARANTINE_ENTRIES;
   if (kept) {
      quarantine[(quarantine_head + quarantine_count) % HC_QUARANTINE_ENTRIES] = b;
      quarantine_count++;
      quarantine_bytes += b->size;
   }
   pthread_mutex_unlock(&quarantine_lock);
   for (unsigned i = 0; i < n; i++) {
      hc_check_freed(evicted[i]);
      hc_release(evicted[i]);
   }
   if (!kept)
      hc_release(b);
}

/* ------------------------------------------------------------------ scans */

/* Every live block's guards, then every kept-back block's fill. `locked`: under the locks (the checking thread);
 * without them from the abort handler, stopping at a list that changes underneath. */
static void
hc_scan(bool locked, bool then_abort)
{
   pthread_once(&shards_once, shards_init);
   uint64_t blocks = 0;
   for (unsigned i = 0; i < HC_SHARDS; i++) {
      struct hc_shard *const s = &shards[i];
      if (locked)
         pthread_mutex_lock(&s->lock);
      for (const struct hc_block *b = s->head; b; b = b->next) {
         if (!hc_plausible(b)) {
            if (locked)
               hc_report("a live block's header is overwritten (its list pointer is damaged)", NULL, b, NULL,
                         then_abort);
            break;
         }
         if (b->magic == HC_FREED && !locked)
            break; /* freed meanwhile */
         if (b->magic != HC_LIVE) {
            hc_report("a live block's header is overwritten", b, user_of(b), (const uint8_t *)&b->magic,
                      then_abort);
            break;
         }
         const uint8_t *bad = hc_first_not(b->front, HC_FRONT, HC_FRONT_BYTE);
         if (bad)
            hc_report("found by the heap scan: written before a live block's start", b, user_of(b), bad, then_abort);
         else if ((bad = hc_first_not(user_of(b) + b->size, HC_TAIL, HC_TAIL_BYTE)))
            hc_report("found by the heap scan: written past a live block's end", b, user_of(b), bad, then_abort);
         blocks++;
      }
      if (locked)
         pthread_mutex_unlock(&s->lock);
   }
   if (locked)
      pthread_mutex_lock(&quarantine_lock);
   for (unsigned i = 0; i < quarantine_count; i++) {
      const struct hc_block *const b = quarantine[(quarantine_head + i) % HC_QUARANTINE_ENTRIES];
      if (!hc_plausible(b) || b->magic != HC_FREED)
         break;
      const uint8_t *bad = hc_first_not(b->front, HC_FRONT, HC_FRONT_BYTE);
      if (!bad)
         bad = hc_first_not(user_of(b), b->size, HC_FREED_BYTE);
      if (!bad)
         bad = hc_first_not(user_of(b) + b->size, HC_TAIL, HC_TAIL_BYTE);
      if (bad) {
         if (locked)
            pthread_mutex_unlock(&quarantine_lock);
         hc_report("found by the heap scan: written after free", b, user_of(b), bad, then_abort);
         return;
      }
   }
   if (locked)
      pthread_mutex_unlock(&quarantine_lock);
   __atomic_store_n(&last_pass_blocks, blocks, __ATOMIC_RELAXED);
}

static void *
hc_thread(void *unused)
{
   (void)unused;
   for (;;) {
      struct timespec two = {.tv_sec = 2, .tv_nsec = 0};
      nanosleep(&two, NULL);
      hc_scan(true, true);
      __atomic_add_fetch(&passes, 1, __ATOMIC_RELAXED);
   }
   return NULL;
}

/* From main: the thread that scans every 2 seconds. Returns how long one call stack takes to record, in
 * nanoseconds, for the log (every allocation records one). */
uint64_t
reeye_heapcheck_start(void)
{
   pthread_once(&shards_once, shards_init);
   __atomic_store_n(&stacks_on, 1, __ATOMIC_RELEASE);
   uintptr_t frames[HC_ALLOC_FRAMES];
   struct timespec t0, t1;
   clock_gettime(CLOCK_MONOTONIC, &t0);
   for (int i = 0; i < 200; i++)
      hc_capture(frames, HC_ALLOC_FRAMES, 0);
   clock_gettime(CLOCK_MONOTONIC, &t1);
   pthread_t thread;
   pthread_attr_t attributes;
   pthread_attr_init(&attributes);
   pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
   pthread_create(&thread, &attributes, hc_thread, NULL);
   pthread_attr_destroy(&attributes);
   return (uint64_t)((t1.tv_sec - t0.tv_sec) * 1000000000LL + (t1.tv_nsec - t0.tv_nsec)) / 200;
}

/* From the abort handler: one scan, written to the report if it finds damage (no second abort) */
void
reeye_heapcheck_scan(void)
{
   if (!__atomic_load_n(&reported, __ATOMIC_ACQUIRE))
      hc_scan(false, false);
}

/* For the log: allocations so far, scans so far, blocks the last one checked, blocks live now, bytes kept back.
 * Returns whether the checks are built in. */
int
reeye_heapcheck_stats(uint64_t *allocations, uint64_t *scans, uint64_t *checked, uint64_t *live,
                      uint64_t *kept_bytes)
{
   *allocations = __atomic_load_n(&next_serial, __ATOMIC_RELAXED);
   *scans = __atomic_load_n(&passes, __ATOMIC_RELAXED);
   *checked = __atomic_load_n(&last_pass_blocks, __ATOMIC_RELAXED);
   *live = __atomic_load_n(&live_blocks, __ATOMIC_RELAXED);
   *kept_bytes = quarantine_bytes;
   return 1;
}

/* ------------------------------------------------------- the wrapped calls */

/* Each wrap calls hc_alloc or hc_free itself, never as a tail call, so a block's stack starts at the wrap */
#define HC_KEEP(x) __asm__ volatile("" : "+r"(x))

void *
__wrap_malloc(size_t bytes)
{
   void *p = hc_alloc(bytes, 0);
   HC_KEEP(p);
   if (p)
      return p;
   ps5p_heap_count_fallback();
   return __real_malloc(bytes);
}

void *
__wrap_calloc(size_t count, size_t bytes)
{
   if (bytes && count > SIZE_MAX / bytes) {
      errno = ENOMEM;
      return NULL;
   }
   void *p = hc_alloc(count * bytes, 0);
   HC_KEEP(p);
   if (p) {
      memset(p, 0, count * bytes);
      return p;
   }
   ps5p_heap_count_fallback();
   return __real_calloc(count, bytes);
}

void
__wrap_free(void *pointer)
{
   if (!pointer)
      return;
   if (ps5_heap_owns(pointer)) {
      hc_free(pointer, "free");
      HC_KEEP(pointer);
   } else {
      __real_free(pointer);
   }
}

size_t
__wrap_malloc_usable_size(const void *pointer)
{
   if (!pointer)
      return 0;
   if (ps5_heap_owns(pointer)) {
      const struct hc_block *const b = header_of(pointer);
      if (((uintptr_t)pointer & (HC_ALIGN - 1)) || !hc_plausible(b))
         hc_report("malloc_usable_size of a pointer the heap never gave out", NULL, pointer, NULL, true);
      hc_check(b, "malloc_usable_size");
      return b->size;
   }
   return __real_malloc_usable_size ? __real_malloc_usable_size(pointer) : 0;
}

void *
__wrap_realloc(void *pointer, size_t bytes)
{
   if (!pointer) {
      void *p = hc_alloc(bytes, 0);
      HC_KEEP(p);
      if (p)
         return p;
      ps5p_heap_count_fallback();
      return __real_malloc(bytes);
   }
   /* FreeBSD's realloc gives a zero-size request a minimum-size object */
   if (bytes == 0)
      bytes = 1;
   if (ps5_heap_owns(pointer)) {
      const struct hc_block *const b = header_of(pointer);
      if (((uintptr_t)pointer & (HC_ALIGN - 1)) || !hc_plausible(b))
         hc_report("realloc of a pointer the heap never gave out", NULL, pointer, NULL, true);
      hc_check(b, "realloc");
      const size_t kept = b->size < bytes ? b->size : bytes;
      void *moved = hc_alloc(bytes, 0);
      HC_KEEP(moved);
      if (!moved) {
         moved = __real_malloc(bytes);
         if (!moved)
            return NULL;
         ps5p_heap_count_fallback();
      }
      memcpy(moved, pointer, kept);
      hc_free(pointer, "realloc");
      return moved;
   }
   void *const moved = __real_realloc(pointer, bytes);
   if (moved || !__real_malloc_usable_size)
      return moved;
   /* libc's heap is exhausted: its block moves to the title heap */
   void *copy = hc_alloc(bytes, 0);
   HC_KEEP(copy);
   if (!copy)
      return NULL;
   const size_t kept = __real_malloc_usable_size(pointer);
   memcpy(copy, pointer, kept < bytes ? kept : bytes);
   __real_free(pointer);
   return copy;
}

void *
__wrap_reallocf(void *pointer, size_t bytes)
{
   void *const moved = __wrap_realloc(pointer, bytes);
   if (!moved)
      __wrap_free(pointer);
   return moved;
}

void *
__wrap_reallocarray(void *pointer, size_t count, size_t bytes)
{
   if (bytes != 0 && count > SIZE_MAX / bytes) {
      errno = ENOMEM;
      return NULL;
   }
   return __wrap_realloc(pointer, count * bytes);
}

int
__wrap_posix_memalign(void **out_pointer, size_t alignment, size_t bytes)
{
   if (alignment < sizeof(void *) || (alignment & (alignment - 1)) != 0)
      return EINVAL;
   void *p = hc_alloc(bytes, alignment);
   HC_KEEP(p);
   if (p) {
      *out_pointer = p;
      return 0;
   }
   ps5p_heap_count_fallback();
   return __real_posix_memalign(out_pointer, alignment, bytes);
}

void *
__wrap_aligned_alloc(size_t alignment, size_t bytes)
{
   if (alignment < sizeof(void *))
      alignment = sizeof(void *);
   if ((alignment & (alignment - 1)) != 0) {
      errno = EINVAL;
      return NULL;
   }
   void *p = hc_alloc(bytes, alignment);
   HC_KEEP(p);
   if (p)
      return p;
   ps5p_heap_count_fallback();
   if (__real_posix_memalign(&p, alignment, bytes) != 0) {
      errno = ENOMEM;
      return NULL;
   }
   return p;
}

void *
__wrap_memalign(size_t alignment, size_t bytes)
{
   return __wrap_aligned_alloc(alignment, bytes);
}

/* libc's getdelim grows the caller's buffer with libc's realloc, which must never see a title-heap block: this one
 * grows it with the wrap (as heap_wrap.c does) */
ssize_t
__wrap_getdelim(char **line, size_t *capacity, int delimiter, FILE *stream)
{
   if (!line || !capacity || !stream) {
      errno = EINVAL;
      return -1;
   }
   if (!*line)
      *capacity = 0;
   size_t length = 0;
   for (;;) {
      const int c = fgetc(stream);
      if (c == EOF) {
         if (length == 0 || ferror(stream))
            return -1;
         break;
      }
      if (length + 2 > *capacity) {
         size_t grown = *capacity < 64 ? 128 : *capacity * 2;
         if (grown > (size_t)SSIZE_MAX) {
            errno = EOVERFLOW;
            return -1;
         }
         char *const moved = __wrap_realloc(*line, grown);
         if (!moved)
            return -1;
         *line = moved;
         *capacity = grown;
      }
      (*line)[length++] = (char)c;
      if (c == (unsigned char)delimiter)
         break;
   }
   (*line)[length] = '\0';
   return (ssize_t)length;
}

ssize_t
__wrap_getline(char **line, size_t *capacity, FILE *stream)
{
   return __wrap_getdelim(line, capacity, '\n', stream);
}

#else /* REEYE_HEAPCHECK */

/* Built without the checks: the payload SDK's wraps serve the heap, and these do nothing */
#include <stdint.h>

uint64_t
reeye_heapcheck_start(void)
{
   return 0;
}

void
reeye_heapcheck_scan(void)
{
}

int
reeye_heapcheck_stats(uint64_t *allocations, uint64_t *scans, uint64_t *checked, uint64_t *live,
                      uint64_t *kept_bytes)
{
   *allocations = *scans = *checked = *live = *kept_bytes = 0;
   return 0;
}

#endif /* REEYE_HEAPCHECK */
