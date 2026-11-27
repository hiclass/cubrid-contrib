/* t30: owner -> thread slot -> OS thread lookups follow the thread tables
 * within a window (no server).  The tables are rebuilt once a second; a
 * window may be much longer (--interval).
 *   1 an owner not in the tables yet (a worker just started): after the
 *     tables are rebuilt it is found, without waiting for another owner
 *   2 a slot that went to another thread (a worker ended on its idle
 *     timeout, the entry taken again): the entry of the window keeps the
 *     first thread's CPU ticks and readings; the other thread's count
 *     nowhere
 *   3 a pthread id used again by a thread in another slot: after the
 *     tables are rebuilt the owner is that slot's thread
 * Target reads are replaced (ld --wrap). */
#include "../src/ctv_sampler.c"

#define OWNER_X 0x7f000000a000ULL

static double g_now = 100.0;
static unsigned g_gen = 1;
static int g_slot_of_x = -1;	/* slot of OWNER_X in the tables */
static int g_tid_of[32];	/* OS tid of a slot */

double
__wrap_mono_s (void)
{
  return g_now;
}

unsigned
__wrap_thr_cache_gen (void)
{
  return g_gen;
}

int
__wrap_thr_index_of_tid_mem (uint64_t tid)
{
  return tid == OWNER_X ? g_slot_of_x : -1;
}

int
__wrap_thr_tid_of (int slot)
{
  return slot >= 0 && slot < 32 ? g_tid_of[slot] : 0;
}

int
__wrap_os_task_comm (int pid, int tid, char *out, size_t n)
{
  (void) pid;
  snprintf (out, n, "thread-%d", tid);
  return 0;
}

/* CPU ticks tell the threads apart: tid * 1000 + reads so far */
int
__wrap_os_task_state (int pid, int tid, char *state, long *ticks)
{
  static int reads;

  (void) pid;
  *state = 'R';
  *ticks = (long) tid * 1000 + reads++;
  return 0;
}

int
__wrap_cs_owner_now (uint64_t cs_base, const CS_LAYOUT * l, int i, uint64_t * owner)
{
  memcpy (owner, (const unsigned char *) (uintptr_t) cs_base + (size_t) i * l->stride + l->off_owner, 8);
  return 0;
}

static CS_LAYOUT lo;

static void
hold (LIVE_WIN * w, uint64_t owner)
{
  unsigned char *p = w->blob;
  int32_t rwlock = -1, tran = 0;

  memset (p + lo.off_rwlock, 0, 12);
  memcpy (p + lo.off_rwlock, &rwlock, 4);
  memcpy (p + lo.off_owner, &owner, 8);
  memcpy (p + lo.off_tran, &tran, 4);
}

static void
run (LIVE_WIN * w, int samples)
{
  int k;

  for (k = 0; k < samples; k++)
    {
      g_now += 0.05;
      hold (w, OWNER_X);
      live_accumulate (w, &lo);
    }
}

static const OS_THR *
os_of (const LIVE_WIN * w, int slot)
{
  int j;

  for (j = 0; j < w->nos; j++)
    {
      if (w->os[j].slot == slot && w->os[j].cs == 0)
	{
	  return &w->os[j];
	}
    }
  return NULL;
}

/* samples of the HELD activity of tran 0 seen with thread slot thr */
static int
held (const LIVE_WIN * w, int thr)
{
  int k, n = 0;

  for (k = 0; k < w->st.nact; k++)
    {
      if (w->st.act[k].kind == ACT_HELD && w->st.act[k].cs == 0 && w->st.act[k].thr == thr)
	{
	  n += w->st.act[k].samples;
	}
    }
  return n;
}

#define CHECK(c, ...) do { if (!(c)) { printf ("t30 FAIL: " __VA_ARGS__); printf ("\n"); bad = 1; } } while (0)

int
main (void)
{
  LIVE_WIN *w;
  const OS_THR *o;
  int bad = 0;

  memset (&lo, 0, sizeof (lo));
  lo.stride = 160;
  lo.count = 1;
  lo.off_stats = 152;
  lo.off_rwlock = lo.off_stats - CS_STATS_TO_RWLOCK;
  lo.off_owner = lo.off_stats - CS_STATS_TO_OWNER;
  lo.off_tran = lo.off_stats - 8;
  lo.off_wq = lo.off_stats - CS_STATS_TO_WQ;
  lo.off_pq = lo.off_stats - CS_STATS_TO_PQ;
  g_tid_of[10] = 1010;
  g_tid_of[12] = 1012;
  w = live_win_new (lo.count, lo.stride, 1, 1);	/* 1 Hz: every sample is read */
  w->no_disk = 1;
  w->cs_base = (uint64_t) (uintptr_t) w->blob;

  /* 1: 10 samples unknown, tables rebuilt, 10 samples known - one owner throughout */
  live_win_reset (w, lo.count);
  memset (w->blob, 0, (size_t) lo.stride * lo.count);
  g_slot_of_x = -1;
  run (w, 10);
  g_slot_of_x = 10;
  g_gen++;
  run (w, 10);
  o = os_of (w, 10);
  CHECK (held (w, -1) == 10 && held (w, 10) == 10,
	 "1: holds without a thread %d, as slot 10 %d (want 10 and 10)", held (w, -1), held (w, 10));
  CHECK (o != NULL && o->samples == 10, "1: readings of slot 10: %d (want 10)", o != NULL ? o->samples : -1);

  /* 2: slot 10 is thread 1010 for 10 samples, then thread 2010 */
  live_win_reset (w, lo.count);
  g_tid_of[10] = 1010;
  run (w, 10);
  g_tid_of[10] = 2010;
  g_gen++;
  run (w, 10);
  o = os_of (w, 10);
  CHECK (o != NULL && o->tid == 1010 && o->samples == 10,
	 "2: the entry is of tid %d with %d readings (want 1010, 10)", o != NULL ? o->tid : -1, o != NULL ? o->samples : -1);
  CHECK (o != NULL && o->ticks0 / 1000 == 1010 && o->ticks1 / 1000 == 1010,
	 "2: CPU ticks of two threads in one entry: first of tid %ld, last of tid %ld",
	 o != NULL ? o->ticks0 / 1000 : -1, o != NULL ? o->ticks1 / 1000 : -1);
  g_tid_of[10] = 1010;

  /* 3: the pthread id is slot 10's, then (tables rebuilt) slot 12's */
  live_win_reset (w, lo.count);
  g_slot_of_x = 10;
  run (w, 10);
  g_slot_of_x = 12;
  g_gen++;
  run (w, 10);
  CHECK (held (w, 10) == 10 && held (w, 12) == 10,
	 "3: holds as slot 10 %d, as slot 12 %d (want 10 and 10)", held (w, 10), held (w, 12));
  o = os_of (w, 12);
  CHECK (o != NULL && o->tid == 1012 && o->samples == 10, "3: readings of slot 12: %d (want 10)", o != NULL ? o->samples : -1);
  if (!bad)
    {
      printf ("t30 PASS: an owner found once the thread tables know it; a slot taken by another thread "
	      "keeps its entry apart; a thread id used again follows the tables\n");
    }
  return bad;
}
