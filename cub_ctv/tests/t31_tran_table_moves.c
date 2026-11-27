/* t31: the transaction table is read again every window (no server).
 * The tool reads this process as its target: a log_Gl image whose pointer
 * names an array of descriptor pointers, as the engine keeps all_tdes.
 *   1 the probe finds the array and counts its descriptors
 *   2 nothing changed: the recheck changes nothing
 *   3 the engine reallocated the array elsewhere, larger (restart recovery
 *     with more transactions than slots; the old block is freed): the new
 *     array and its count are used, the pointer image is of the new array
 *   4 the array grew in place (same address): the new count
 *   5 the array is gone (shutdown: pointer cleared): no table
 *   6 it is back: the table again
 * After each change the CLIENTIDS lock scan runs over the new count (its
 * arrays follow it: the sanitizer run of this test checks their bounds). */
#include "../src/ctv.h"
#include <sys/mman.h>

int tran_table_recheck (void) __attribute__((weak));

#include "../src/ctv_tran.c"

#define TDES_BYTES 1024
#define OFF_TI 16
#define SLOTS 4096

static unsigned char *tdes_pool;

static uint64_t *
array_of (int n)
{
  uint64_t *a = mmap (NULL, SLOTS * 8, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  int k;

  for (k = 0; k < n; k++)
    {
      a[k] = (uint64_t) (uintptr_t) (tdes_pool + (size_t) k * TDES_BYTES);
    }
  return a;
}

#define CHECK(c, ...) do { if (!(c)) { printf ("t31 FAIL: " __VA_ARGS__); printf ("\n"); return 1; } } while (0)

int
main (void)
{
  unsigned char *log_gl = mmap (NULL, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  uint64_t *a1, *a2, **slot = (uint64_t **) (log_gl + 0x80);
  uint64_t tdes;
  int k;

  tdes_pool = mmap (NULL, (size_t) SLOTS * TDES_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  for (k = 0; k < SLOTS; k++)
    {
      int32_t ti = k, trid = k == 0 ? 0 : -1;

      memcpy (tdes_pool + (size_t) k * TDES_BYTES + OFF_TI, &ti, 4);
      memcpy (tdes_pool + (size_t) k * TDES_BYTES + OFF_TI + 4, &trid, 4);
    }
  CHECK (mem_open (getpid ()) == 0, "cannot read this process");
  a1 = array_of (100);
  *slot = a1;

  /* 1 */
  CHECK (tran_probe ((uint64_t) (uintptr_t) log_gl) == NULL, "the probe refused the table");
  CHECK (tran_geom.all_tdes == (uint64_t) (uintptr_t) a1 && tran_geom.ntran == 100 && tran_geom.off_ti == OFF_TI,
	 "probe: array %llx count %d off %d", (unsigned long long) tran_geom.all_tdes, tran_geom.ntran, tran_geom.off_ti);
  CHECK (tran_table_recheck != NULL, "the table is never read again after the probe");
  (void) tran_lock_clientids (tran_geom.ntran, 0);	/* its arrays are sized by the count of now */

  /* 2 */
  CHECK (tran_table_recheck () == 0 && tran_geom.ntran == 100, "an unchanged table reported as changed");

  /* 3 */
  a2 = array_of (300);
  *slot = a2;
  memset (a1, 0x5a, SLOTS * 8);	/* the freed block holds anything */
  CHECK (tran_table_recheck () == 1, "a moved table not seen");
  CHECK (tran_geom.all_tdes == (uint64_t) (uintptr_t) a2 && tran_geom.ntran == 300,
	 "moved: array %llx (want %llx) count %d (want 300)", (unsigned long long) tran_geom.all_tdes,
	 (unsigned long long) (uintptr_t) a2, tran_geom.ntran);
  (void) tdes_ptrs_load ();
  (void) tran_lock_clientids (tran_geom.ntran, 16);	/* no client yet: the lazy scan over 300 slots */
  CHECK (ptrs_n == 300 && tdes_at (250, &tdes) == 0 && tdes == a2[250], "slot 250 of the new array is not read (image of %d)", ptrs_n);

  /* 4 */
  for (k = 300; k < 450; k++)
    {
      a2[k] = (uint64_t) (uintptr_t) (tdes_pool + (size_t) k * TDES_BYTES);
    }
  CHECK (tran_table_recheck () == 1 && tran_geom.ntran == 450, "grown in place: count %d (want 450)", tran_geom.ntran);
  (void) tdes_ptrs_load ();	/* the pointer image must have room for 450 */
  (void) tran_lock_clientids (tran_geom.ntran, 0);
  CHECK (ptrs_n == 450 && tdes_at (449, &tdes) == 0 && tdes == a2[449], "slot 449 after the growth is not read (image of %d)", ptrs_n);

  /* 5 */
  *slot = NULL;
  CHECK (tran_table_recheck () == 1 && tran_geom.all_tdes == 0 && tran_geom.ntran == 0, "a cleared pointer still gives a table");
  (void) tdes_ptrs_load ();
  CHECK (tran_table_recheck () == 0, "no table, nothing changed: reported as changed");

  /* 6 */
  *slot = a2;
  CHECK (tran_table_recheck () == 1 && tran_geom.all_tdes == (uint64_t) (uintptr_t) a2 && tran_geom.ntran == 450,
	 "the table back: count %d", tran_geom.ntran);
  printf ("t31 PASS: the transaction table moved, grown in place, gone and back is followed every window\n");
  return 0;
}
