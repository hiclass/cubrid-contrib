/* t32: `--trans -i` and a transaction table that is away for a while
 * (no server).  The live loop runs on the table alone; the window capture
 * is replaced by a script of table states, and the "server" is a child
 * process the test starts and ends.
 *   1 table there, away for three windows (a restart rebuilding it), there
 *     again: every window is shown - "unavailable" while away - and the run
 *     goes on until it is stopped (exit 0)
 *   2 no table at the start (attached during a boot), then there: the run
 *     does not end at the start
 *   3 the server ends while the table is there: the run ends, "server
 *     gone", exit 1 - the one way it ends by itself */
#include "../src/ctv_live.c"
#include <sys/wait.h>

static double g_now = 1000.0;
static const char *g_script;	/* 'T' table there, '-' away, 'X' the server ends */
static int g_win, g_server;

double
__wrap_mono_s (void)
{
  return g_now;
}

void
__wrap_sleep_until (double t)
{
  g_now = t > g_now ? t : g_now;
}

int
__wrap_term_stop_requested (void)
{
  return g_script[g_win] == '\0';	/* the script done: as a Ctrl-C */
}

void
__wrap_view_capture_trans (double dt)
{
  char c = g_script[g_win] != '\0' ? g_script[g_win] : 'T';

  if (c == 'X' && g_server > 0)
    {
      kill (g_server, SIGKILL);
      waitpid (g_server, NULL, 0);	/* reaped: /proc/<pid> is gone */
      g_server = 0;
    }
  view.ts = 0;
  view.dt = dt;
  view.st.samples = 0;
  view.tran_avail = c != '-';
  view.ntran = 0;
  view.nident = 0;
  tran_refused = c == '-' ? "I-6 all_tdes not present now" : NULL;
  g_win++;
}

/* runs the loop in a child: its stdout to out, its exit status back */
static int
run (const char *script, char *out, size_t n)
{
  int fd[2], st;
  pid_t pid;
  ssize_t got = 0, k;

  if (pipe (fd) != 0)
    {
      return -1;
    }
  pid = fork ();
  if (pid == 0)
    {
      VIEW_HDR h;

      dup2 (fd[1], 1);
      dup2 (fd[1], 2);
      close (fd[0]);
      g_server = fork ();
      if (g_server == 0)
	{
	  pause ();
	  _exit (0);
	}
      memset (&h, 0, sizeof (h));
      h.pid = g_server;
      h.live = 1;
      h.hz = 100;
      h.interval = 1.0;
      h.lo.count = 1;
      h.lo.stride = 160;
      snprintf (h.db, sizeof (h.db), "t32");
      g_script = script;
      g_win = 0;
      st = live_mode (0, &h, 0, 1, NULL, 1);
      if (g_server > 0)
	{
	  kill (g_server, SIGKILL);
	  waitpid (g_server, NULL, 0);
	}
      fflush (stdout);
      _exit (st);
    }
  close (fd[1]);
  while ((k = read (fd[0], out + got, n - 1 - (size_t) got)) > 0)
    {
      got += k;
    }
  out[got] = '\0';
  close (fd[0]);
  waitpid (pid, &st, 0);
  return WIFEXITED (st) ? WEXITSTATUS (st) : 128 + WTERMSIG (st);
}

static int
count (const char *s, const char *what)
{
  int n = 0;

  while ((s = strstr (s, what)) != NULL)
    {
      n++;
      s++;
    }
  return n;
}

#define CHECK(c, ...) do { if (!(c)) { printf ("t32 FAIL: " __VA_ARGS__); printf ("\n%s\n", out); return 1; } } while (0)

int
main (void)
{
  static char out[1 << 16];
  const char *away = "transaction table unavailable: I-6 all_tdes not present now";
  int rc;

  setenv ("LANG", "C", 1);
  /* 1: the startup capture, then windows T T - - - T T */
  rc = run ("TTT---TT", out, sizeof (out));
  CHECK (rc == 0, "1: a table away for three windows ended the run (exit %d)", rc);
  CHECK (count (out, "--- ") == 7, "1: %d windows shown, want 7", count (out, "--- "));
  CHECK (count (out, away) == 3, "1: 'unavailable' shown %d times, want 3", count (out, away));
  CHECK (strstr (out, "server gone") == NULL, "1: said the server is gone");

  /* 2: no table at the start, then there */
  rc = run ("--TT", out, sizeof (out));
  CHECK (rc == 0, "2: no table at the start ended the run (exit %d)", rc);
  CHECK (count (out, away) == 2 && count (out, "--- ") == 4, "2: %d unavailable, %d windows (want 2, 4: the startup one shown too)",
	 count (out, away), count (out, "--- "));

  /* 3: the server ends */
  rc = run ("TTXTT", out, sizeof (out));
  CHECK (rc == 1, "3: the server ended, exit %d (want 1)", rc);
  CHECK (strstr (out, "target read failed (server gone?)") != NULL, "3: no 'server gone'");
  CHECK (count (out, away) == 0, "3: the end shown as a table away");
  printf ("t32 PASS: --trans -i shows a table away (restart) and goes on, from the start too; "
	  "only a server that ends ends the run\n");
  return 0;
}
