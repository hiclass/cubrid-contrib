/*
 * t26: a statistics block read while the engine adds to total_elapsed.  The
 * engine stores tv_sec += e.sec, tv_usec += e.usec, tv_sec += the carry,
 * tv_usec %= 1e6 in that order; tv_usec >= 1e6 is seen between them, and
 * after the third store the sum reads 1 s too much.  Such a block is read
 * again, at most 3 times: a whole block replaces it whole, of torn ones the
 * smallest sum is kept.  No other block is read again.
 */
#include "../src/ctv.h"

#define ST0 0x10000ULL		/* stats block of CS 0; CS 1 follows */
#define END -9

typedef struct
{
  long sec, usec;		/* sec END: no more; sec < 0: the read fails */
} TV;

static unsigned char first[2][STATS_SIZE];
static const TV *later;
static int torn_cs, rereads[2];

static void
put_stats (unsigned char *s, long sec, long usec, uint32_t nenter)
{
  int32_t type = 1;
  int64_t tv[2] = { sec, usec };

  memset (s, 0, STATS_SIZE);
  memcpy (s + ST_OFF_TYPE, &type, 4);
  memcpy (s + ST_OFF_NENTER, &nenter, 4);
  memcpy (s + ST_OFF_TOTAL_TV, tv, 16);
}

int
__wrap_mem_gather_ok (const uint64_t * addrs, size_t n, size_t each, void *buf, unsigned char *ok)
{
  size_t i;

  for (i = 0; i < n; i++)
    {
      memcpy ((char *) buf + i * each, first[addrs[i] == ST0 ? 0 : 1], each);
    }
  return 0;
}

/* the k-th re-read of the torn CS's block answers later[k] with nenter
 * 501 + k; a re-read of the other block is counted and fails */
int
__wrap_mem_read (uint64_t addr, void *buf, size_t n)
{
  int cs = addr == ST0 ? 0 : addr == ST0 + STATS_SIZE ? 1 : -1, k;

  if (n != STATS_SIZE || cs < 0)
    {
      return -1;		/* names and the rest: not readable here */
    }
  k = rereads[cs]++;
  if (cs != torn_cs || later[k].sec < 0)
    {
      return -1;
    }
  put_stats (buf, later[k].sec, later[k].usec, 501 + (uint32_t) k);
  return 0;
}

static int
scene (const char *what, int cs, long sec, long usec, const TV * seq,
       double want_ms, uint32_t want_nenter, int want_rereads)
{
  CS_LAYOUT lo;
  CS_ROW rows[2];
  unsigned char blob[2 * 160];
  uint64_t a;
  int i, ok, o = !cs;

  memset (&lo, 0, sizeof (lo));
  lo.stride = 160;
  lo.count = 2;
  lo.off_stats = 152;
  lo.off_rwlock = lo.off_stats - CS_STATS_TO_RWLOCK;
  lo.off_owner = lo.off_stats - CS_STATS_TO_OWNER;
  lo.off_tran = lo.off_stats - 8;
  lo.off_wq = lo.off_stats - CS_STATS_TO_WQ;
  lo.off_pq = lo.off_stats - CS_STATS_TO_PQ;
  memset (blob, 0, sizeof (blob));
  for (i = 0; i < 2; i++)
    {
      a = ST0 + (uint64_t) i * STATS_SIZE;
      memcpy (blob + i * lo.stride + lo.off_stats, &a, 8);
    }
  put_stats (first[cs], sec, usec, 500);
  put_stats (first[o], 3, 999999, 77);	/* the largest whole tv_usec */
  later = seq;
  torn_cs = cs;
  rereads[0] = rereads[1] = 0;
  (void) cs_rows_from_blob (blob, &lo, rows);
  ok = rows[cs].has_stats && rows[cs].total_ms == want_ms && rows[cs].nenter == want_nenter
    && rereads[cs] == want_rereads
    && rows[o].has_stats && rows[o].total_ms == 3999.999 && rows[o].nenter == 77 && rereads[o] == 0;
  printf ("  CS %d %-50s total %.3f ms, nenter %u, re-reads %d%s\n", cs, what, rows[cs].total_ms,
	  rows[cs].nenter, rereads[cs], ok ? "" : "   <-- wrong");
  return !ok;
}

int
main (void)
{
  /* 9.9 s + 0.3 s: 9/1200000 (exact), 10/1200000 (the carry in: 11.2 s), 10/200000 */
  static const TV none[] = { {END, 0} };
  static const TV whole[] = { {10, 200000}, {END, 0} };
  static const TV whole0[] = { {10, 0}, {END, 0} };
  static const TV late[] = { {10, 1200000}, {10, 1200000}, {10, 200000}, {END, 0} };
  static const TV stuck[] = { {10, 1200000}, {10, 1200000}, {10, 1200000}, {10, 200000}, {END, 0} };
  static const TV next[] = { {10, 1100000}, {10, 1100000}, {10, 1100000}, {END, 0} };
  static const TV fails[] = { {-1, 0}, {10, 200000}, {END, 0} };
  static const TV fails2[] = { {10, 1200000}, {-1, 0}, {10, 200000}, {END, 0} };
  int bad = 0;

  bad += scene ("whole block", 0, 10, 200000, none, 10200.0, 500, 0);
  bad += scene ("carry in; whole on the re-read", 0, 10, 1200000, whole, 10200.0, 501, 1);
  bad += scene ("carry in; whole on the re-read", 1, 10, 1200000, whole, 10200.0, 501, 1);
  bad += scene ("no carry yet; whole on the re-read", 0, 9, 1200000, whole, 10200.0, 501, 1);
  bad += scene ("usec exactly 1e6; whole on the re-read", 0, 9, 1000000, whole0, 10000.0, 501, 1);
  bad += scene ("carry in; whole on the third re-read", 0, 10, 1200000, late, 10200.0, 503, 3);
  bad += scene ("carry in on all four: the +1 s stays", 0, 10, 1200000, stuck, 11200.0, 500, 3);
  bad += scene ("no carry, then the carry in: the first", 0, 9, 1200000, stuck, 10200.0, 500, 3);
  bad += scene ("carry in, then a smaller torn sum: that one", 0, 10, 1200000, next, 11100.0, 501, 3);
  bad += scene ("the re-read fails: kept as read", 0, 10, 1200000, fails, 11200.0, 500, 1);
  bad += scene ("the second re-read fails: kept as read", 0, 10, 1200000, fails2, 11200.0, 500, 2);
  if (bad || STATS_REREAD != 3)
    {
      printf ("t26 FAIL: %d scenes%s\n", bad, STATS_REREAD != 3 ? "; STATS_REREAD is not 3" : "");
      return 1;
    }
  printf ("t26 PASS: a time sum with tv_usec >= 1e6 is read again (at most 3 times; of torn "
	  "readings the smallest), others are not\n");
  return 0;
}
