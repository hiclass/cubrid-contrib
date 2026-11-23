/*
 * t23: the samples a holder's procfs state is read on must not depend on
 * where in a hold they fall.  A thread holds LOG, first on a CPU and then
 * in disk sleep, and rests between holds; the D share of its readings must
 * be that of the samples that see it hold.  100 Hz, 400 windows of 100
 * samples, the reading draws seeded: the same run every time.
 *   1 holds of 50 ms (10 R, 40 D) every 110 ms
 *   2 the same holds after rests of 30..70 ms (no fixed period)
 *   3 periods that are multiples of the 50 ms block, at each of the five
 *     offsets from the block's start: 50 ms (30 held: 10 R, 20 D),
 *     100 ms (50 held: 10 R, 40 D), 200 ms (100 held: 20 R, 80 D)
 *   4 a thread holding all the time: one reading in every block of five
 *     samples, each place in the block drawn about as often
 *   5 the same seed gives the same readings, another seed other ones
 *   6 20 Hz and below: every sample is read
 *   7 the fewest samples OS_MIN_READS readings can fall in (os_min_samples):
 *     no window one sample shorter ever holds that many, some window of
 *     that length does; and the fewest samples that always hold that many
 *     when the thread holds throughout ((OS_MIN_READS + 1) blocks less one
 *     sample): every window of that length does, some window one sample
 *     shorter does not - at 100, 50 and 20 Hz
 * Target reads are replaced (ld --wrap): thread ids, procfs, the clock.
 */
#include "../src/ctv_sampler.c"

#define OWNER_A 0x7f000000a000ULL	/* thread slot 10 */
#define SEED 20261120u
#define WINDOWS 400

static double g_now = 100.0;
static int g_k;			/* the sample being taken */
static int g_hold_k;		/* the sample the current hold began in */
static int g_run;		/* samples on a CPU from the start of a hold */
static int g_reads;
static int g_place[5];		/* case 4: readings by place in the block */

double
__wrap_mono_s (void)
{
  return g_now;
}

int
__wrap_thr_index_of_tid_mem (uint64_t tid)
{
  return tid == OWNER_A ? 10 : -1;
}

int
__wrap_thr_tid_of (int slot)
{
  return slot == 10 ? 1010 : 0;
}

int
__wrap_os_task_comm (int pid, int tid, char *out, size_t n)
{
  (void) pid;
  (void) tid;
  snprintf (out, n, "log-flush");
  return 0;
}

static char
state_now (void)
{
  return g_k - g_hold_k < g_run ? 'R' : 'D';
}

int
__wrap_os_task_state (int pid, int tid, char *state, long *ticks)
{
  (void) pid;
  (void) tid;
  g_reads++;
  g_place[g_k % 5]++;
  *state = state_now ();
  *ticks = 500;
  return 0;
}

static CS_LAYOUT lo;
static int bad;

static void
put_cs (LIVE_WIN * w, int i, int32_t rwlock, uint64_t owner, int32_t tran)
{
  unsigned char *p = w->blob + (size_t) i * lo.stride;

  memset (p + lo.off_rwlock, 0, 12);
  memcpy (p + lo.off_rwlock, &rwlock, 4);
  memcpy (p + lo.off_owner, &owner, 8);
  memcpy (p + lo.off_tran, &tran, 4);
}

/* holds of `hold` samples (the first `run` on a CPU) and rests of `rest`
 * samples (rest < 0: 3..7, drawn), the first hold `off` samples into a
 * block; *rd = readings, *rd_d = those in disk sleep.  Returns the D share
 * of the held samples */
static double
scene (int hz, unsigned seed, int hold, int run, int rest, int off, int *rd, int *rd_d)
{
  LIVE_WIN *w = live_win_new (lo.count, lo.stride, hz, seed);
  unsigned lcg = 12345;
  int win, k, held = 0, held_d = 0, left = off, resting = 1;

  w->no_disk = 1;
  g_run = run;
  g_k = 0;
  *rd = *rd_d = 0;
  for (win = 0; win < WINDOWS; win++)
    {
      live_win_reset (w, lo.count);
      memset (w->blob, 0, (size_t) lo.stride * lo.count);
      for (k = 0; k < 100; k++, g_k++)
	{
	  g_now += 1.0 / hz;
	  while (left == 0)
	    {
	      resting = !resting;
	      if (!resting)
		{
		  g_hold_k = g_k;
		  left = hold;
		}
	      else
		{
		  lcg = lcg * 1103515245u + 12345u;
		  left = rest >= 0 ? rest : 3 + (int) ((lcg >> 16) % 5);
		}
	    }
	  left--;
	  if (resting)
	    {
	      put_cs (w, 1, 0, 0, -1);
	    }
	  else
	    {
	      put_cs (w, 1, -1, OWNER_A, -1);
	      held++;
	      held_d += state_now () == 'D';
	    }
	  live_accumulate (w, &lo);
	}
      for (k = 0; k < w->nos; k++)
	{
	  if (w->os[k].slot == 10 && w->os[k].cs == 1)
	    {
	      *rd += w->os[k].samples;
	      *rd_d += w->os[k].dsk;
	    }
	}
    }
  live_win_free (w);
  return held > 0 ? 100.0 * held_d / held : 0.0;
}

/* the D share of the readings is within 5 points of the held samples' */
static void
near (const char *name, int hold, int run, int rest, int off)
{
  int rd, rd_d;
  double want = scene (100, SEED, hold, run, rest, off, &rd, &rd_d);
  double got = rd > 0 ? 100.0 * rd_d / rd : -1.0;

  printf ("  %-34s held samples in disk sleep %5.1f%%, readings %4d in disk sleep %5.1f%%\n",
	  name, want, rd, got);
  if (rd < 1000 || got < want - 5.0 || got > want + 5.0)
    {
      printf ("t23 FAIL: %s: D %.1f%% of %d readings, %.1f%% of the samples it held\n", name, got, rd, want);
      bad = 1;
    }
}

int
main (void)
{
  int rd, rd_d, rd2, rd2_d, off, i;
  char name[64];

  memset (&lo, 0, sizeof (lo));
  lo.stride = 160;
  lo.count = 2;
  lo.off_stats = 152;
  lo.off_rwlock = lo.off_stats - CS_STATS_TO_RWLOCK;
  lo.off_owner = lo.off_stats - CS_STATS_TO_OWNER;
  lo.off_tran = lo.off_stats - 8;
  lo.off_wq = lo.off_stats - CS_STATS_TO_WQ;
  lo.off_pq = lo.off_stats - CS_STATS_TO_PQ;

  near ("1 holds every 110 ms", 5, 1, 6, 0);
  near ("2 rests of 30..70 ms", 5, 1, -1, 0);
  for (off = 0; off < 5; off++)
    {
      snprintf (name, sizeof (name), "3 period  50 ms, offset %d0 ms", off);
      near (name, 3, 1, 2, off);
      snprintf (name, sizeof (name), "3 period 100 ms, offset %d0 ms", off);
      near (name, 5, 1, 5, off);
      snprintf (name, sizeof (name), "3 period 200 ms, offset %d0 ms", off);
      near (name, 10, 2, 10, off);
    }

  /* 4: held all the time (the hold never ends) */
  memset (g_place, 0, sizeof (g_place));
  g_reads = 0;
  (void) scene (100, SEED, WINDOWS * 100, 0, 0, 0, &rd, &rd_d);
  printf ("  4 always held: %d readings in %d samples, by place in the block %d %d %d %d %d\n",
	  rd, WINDOWS * 100, g_place[0], g_place[1], g_place[2], g_place[3], g_place[4]);
  if (rd != WINDOWS * 100 / 5 || g_reads != rd)
    {
      printf ("t23 FAIL: %d readings, %d reads in %d samples (want one in every block of 5: %d)\n",
	      rd, g_reads, WINDOWS * 100, WINDOWS * 100 / 5);
      bad = 1;
    }
  for (i = 0; i < 5; i++)
    {
      /* 8000 draws over 5 places: 1600 each, sd 36 */
      if (g_place[i] < 1600 - 180 || g_place[i] > 1600 + 180)
	{
	  printf ("t23 FAIL: place %d of the block read %d times (want 1600 +- 180)\n", i, g_place[i]);
	  bad = 1;
	}
    }

  /* 5: the seed decides the readings */
  (void) scene (100, SEED, 5, 1, 6, 0, &rd, &rd_d);
  (void) scene (100, SEED, 5, 1, 6, 0, &rd2, &rd2_d);
  if (rd != rd2 || rd_d != rd2_d)
    {
      printf ("t23 FAIL: the same seed read %d/%d then %d/%d\n", rd_d, rd, rd2_d, rd2);
      bad = 1;
    }
  (void) scene (100, SEED + 1, 5, 1, 6, 0, &rd2, &rd2_d);
  printf ("  5 seed %u: %d of %d readings in disk sleep, twice; seed %u: %d of %d\n",
	  SEED, rd_d, rd, SEED + 1, rd2_d, rd2);
  if (rd == rd2 && rd_d == rd2_d)
    {
      printf ("t23 FAIL: another seed gave the same readings (%d/%d)\n", rd_d, rd);
      bad = 1;
    }

  /* 6: 20 Hz and 10 Hz read every sample */
  g_reads = 0;
  (void) scene (20, SEED, WINDOWS * 100, 0, 0, 0, &rd, &rd_d);
  (void) scene (10, SEED, WINDOWS * 100, 0, 0, 0, &rd2, &rd2_d);
  printf ("  6 always held at 20 Hz / 10 Hz: %d / %d readings in %d samples\n", rd, rd2, WINDOWS * 100);
  if (rd != WINDOWS * 100 || rd2 != WINDOWS * 100)
    {
      printf ("t23 FAIL: 20 Hz read %d, 10 Hz %d of %d samples (want all)\n", rd, rd2, WINDOWS * 100);
      bad = 1;
    }
  /* 7: a thread holding all the time, every window of len samples slid
   * over the draws of 20000 samples */
  {
    static const int hzs[] = { 100, 50, 20 };
    static char pick[20000];
    int h, len, at, most[2];

    for (h = 0; h < 3; h++)
      {
	LIVE_WIN *w = live_win_new (lo.count, lo.stride, hzs[h], SEED);
	int need = os_min_samples (hzs[h]);

	w->no_disk = 1;
	live_win_reset (w, lo.count);
	memset (w->blob, 0, (size_t) lo.stride * lo.count);
	for (i = 0; i < 20000; i++)
	  {
	    if (i % 7 == 6)
	      {
		live_win_reset (w, lo.count);	/* blocks run on across windows */
	      }
	    live_accumulate (w, &lo);
	    pick[i] = (char) w->os_pick;
	  }
	live_win_free (w);
	for (len = need - 1; len <= need; len++)
	  {
	    int in = 0;

	    most[len - need + 1] = 0;
	    for (at = 0; at < 20000; at++)
	      {
		in += pick[at];
		if (at >= len)
		  {
		    in -= pick[at - len];
		  }
		if (at >= len - 1 && in > most[len - need + 1])
		  {
		    most[len - need + 1] = in;
		  }
	      }
	  }
	printf ("  7 %3d Hz: block %d, most readings in a window of %d samples %d, of %d samples %d\n",
		hzs[h], os_block_samples (hzs[h]), need - 1, most[0], need, most[1]);
	{
	  int sure = (OS_MIN_READS + 1) * os_block_samples (hzs[h]) - 1, least[2];

	  for (len = sure - 1; len <= sure; len++)
	    {
	      int in = 0;

	      least[len - sure + 1] = len;
	      for (at = 0; at < 20000; at++)
		{
		  in += pick[at];
		  if (at >= len)
		    {
		      in -= pick[at - len];
		    }
		  if (at >= len - 1 && in < least[len - sure + 1])
		    {
		      least[len - sure + 1] = in;
		    }
		}
	    }
	  printf ("    always held: fewest readings in a window of %d samples %d, of %d samples %d\n",
		  sure - 1, least[0], sure, least[1]);
	  if (least[1] < OS_MIN_READS || least[0] >= OS_MIN_READS)
	    {
	      printf ("t23 FAIL: %d Hz: %d samples hold as few as %d readings and %d as few as %d (want >= %d, < %d)\n",
		      hzs[h], sure, least[1], sure - 1, least[0], OS_MIN_READS, OS_MIN_READS);
	      bad = 1;
	    }
	}
	if (most[0] >= OS_MIN_READS || most[1] < OS_MIN_READS)
	  {
	    printf ("t23 FAIL: %d Hz: os_min_samples %d, but %d samples hold up to %d readings and %d up to %d (want < %d, >= %d)\n",
		    hzs[h], need, need - 1, most[0], need, most[1], OS_MIN_READS, OS_MIN_READS);
	    bad = 1;
	  }
      }
  }
  if (!bad)
    {
      printf ("t23 PASS: readings follow the held samples (periods of 50 ms multiples too), one in every block\n");
    }
  return bad;
}
