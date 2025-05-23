/* This testcase is part of GDB, the GNU debugger.

   Copyright 2010-2025 Free Software Foundation, Inc.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <limits.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <signal.h>
#include <unistd.h>
#include <asm/unistd.h>

#define gettid() syscall (__NR_gettid)
#define tgkill(tgid, tid, sig) syscall (__NR_tgkill, tgid, tid, sig)

/* Terminate always in the main task.  It can lock up with SIGSTOPped
   GDB otherwise.  */
#define TIMEOUT (gettid () == getpid() ? 10 : 15)

static pid_t thread1_tid;
static pthread_cond_t thread1_tid_cond
  = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t thread1_tid_mutex
  = PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP;
static int thread1_sigusr1_hit;
static int thread1_sigusr2_hit;

static pid_t thread2_tid;
static pthread_cond_t thread2_tid_cond
  = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t thread2_tid_mutex
  = PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP;
static int thread2_sigusr1_hit;
static int thread2_sigusr2_hit;

static pthread_mutex_t terminate_mutex
  = PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP;

static pthread_barrier_t threads_started_barrier;

/* Do not use alarm as it would create a ptrace event which would hang
   us up if we are being traced by GDB, which we stopped
   ourselves.  */

static void
timed_mutex_lock (pthread_mutex_t *mutex)
{
  int i;
  struct timespec start, now;

  i = clock_gettime (CLOCK_MONOTONIC, &start);
  assert (i == 0);

  do
    {
      i = pthread_mutex_trylock (mutex);
      if (i == 0)
	return;
      assert (i == EBUSY);

      i = clock_gettime (CLOCK_MONOTONIC, &now);
      assert (i == 0);
      assert (now.tv_sec >= start.tv_sec);
    }
  while (now.tv_sec - start.tv_sec < TIMEOUT);

  fprintf (stderr, "Timed out waiting for internal lock!\n");
  exit (EXIT_FAILURE);
}

static void
handler (int signo, siginfo_t *siginfo, void *exception)
{
  int *varp;

  assert (siginfo->si_signo == signo);
  assert (siginfo->si_code == SI_TKILL);
  assert (siginfo->si_pid == getpid ());

  if (gettid () == thread1_tid)
    {
      if (signo == SIGUSR1)
	varp = &thread1_sigusr1_hit;
      else if (signo == SIGUSR2)
	varp = &thread1_sigusr2_hit;
      else
	assert (0);
    }
  else if (gettid () == thread2_tid)
    {
      if (signo == SIGUSR1)
	varp = &thread2_sigusr1_hit;
      else if (signo == SIGUSR2)
	varp = &thread2_sigusr2_hit;
      else
	assert (0);
    }
  else
    assert (0);

  if (*varp)
    {
      fprintf (stderr, "Signal %d for TID %lu has been already hit!\n", signo,
	       (unsigned long) gettid ());
      exit (EXIT_FAILURE);
    }
  *varp = 1;
}

static void *
thread1_func (void *unused)
{
  int i;

  pthread_barrier_wait (&threads_started_barrier);

  timed_mutex_lock (&thread1_tid_mutex);

  /* THREAD1_TID_MUTEX must be already locked to avoid a race.  */
  thread1_tid = gettid ();

  i = pthread_cond_signal (&thread1_tid_cond);
  assert (i == 0);
  i = pthread_mutex_unlock (&thread1_tid_mutex);
  assert (i == 0);

  /* Be sure the "t (tracing stop)" test can proceed for both
     threads.  */
  timed_mutex_lock (&terminate_mutex);
  i = pthread_mutex_unlock (&terminate_mutex);
  assert (i == 0);

  if (!thread1_sigusr1_hit)
    {
      fprintf (stderr, "Thread 1 signal SIGUSR1 not hit!\n");
      exit (EXIT_FAILURE);
    }
  if (!thread1_sigusr2_hit)
    {
      fprintf (stderr, "Thread 1 signal SIGUSR2 not hit!\n");
      exit (EXIT_FAILURE);
    }

  return NULL;
}

static void *
thread2_func (void *unused)
{
  int i;

  pthread_barrier_wait (&threads_started_barrier);

  timed_mutex_lock (&thread2_tid_mutex);

  /* THREAD2_TID_MUTEX must be already locked to avoid a race.  */
  thread2_tid = gettid ();

  i = pthread_cond_signal (&thread2_tid_cond);
  assert (i == 0);
  i = pthread_mutex_unlock (&thread2_tid_mutex);
  assert (i == 0);

  /* Be sure the "t (tracing stop)" test can proceed for both
     threads.  */
  timed_mutex_lock (&terminate_mutex);
  i = pthread_mutex_unlock (&terminate_mutex);
  assert (i == 0);

  if (!thread2_sigusr1_hit)
    {
      fprintf (stderr, "Thread 2 signal SIGUSR1 not hit!\n");
      exit (EXIT_FAILURE);
    }
  if (!thread2_sigusr2_hit)
    {
      fprintf (stderr, "Thread 2 signal SIGUSR2 not hit!\n");
      exit (EXIT_FAILURE);
    }

  return NULL;
}

static const char *
proc_string (const char *filename, const char *line)
{
  FILE *f;
  static char buf[LINE_MAX];
  size_t line_len = strlen (line);

  f = fopen (filename, "r");
  if (f == NULL)
    {
      fprintf (stderr, "fopen (\"%s\") for \"%s\": %s\n", filename, line,
	       strerror (errno));
      exit (EXIT_FAILURE);
    }
  while (errno = 0, fgets (buf, sizeof (buf), f))
    {
      char *s;

      s = strchr (buf, '\n');
      assert (s != NULL);
      *s = 0;

      if (strncmp (buf, line, line_len) != 0)
	continue;

      if (fclose (f))
	{
	  fprintf (stderr, "fclose (\"%s\") for \"%s\": %s\n", filename, line,
		   strerror (errno));
	  exit (EXIT_FAILURE);
	}

      return &buf[line_len];
    }
  if (errno != 0)
    {
      fprintf (stderr, "fgets (\"%s\": %s\n", filename, strerror (errno));
      exit (EXIT_FAILURE);
    }
  fprintf (stderr, "\"%s\": No line \"%s\" found.\n", filename, line);
  exit (EXIT_FAILURE);
}

static unsigned long
proc_ulong (const char *filename, const char *line)
{
  const char *s = proc_string (filename, line);
  long retval;
  char *end;

  errno = 0;
  retval = strtol (s, &end, 10);
  if (retval < 0 || retval >= LONG_MAX || (end && *end))
    {
      fprintf (stderr, "\"%s\":\"%s\": %ld, %s\n", filename, line, retval,
	       strerror (errno));
      exit (EXIT_FAILURE);
    }
  return retval;
}

static void
state_wait (pid_t process, const char *wanted)
{
  char *filename;
  int i;
  struct timespec start, now;
  const char *state;

  i = asprintf (&filename, "/proc/%lu/status", (unsigned long) process);
  assert (i > 0);

  i = clock_gettime (CLOCK_MONOTONIC, &start);
  assert (i == 0);

  do
    {
      state = proc_string (filename, "State:\t");

      /* torvalds/linux-2.6.git 464763cf1c6df632dccc8f2f4c7e50163154a2c0
	 has changed "T (tracing stop)" to "t (tracing stop)".  Make the GDB
	 testcase backward compatible with older Linux kernels.  */
      if (strcmp (state, "T (tracing stop)") == 0)
	state = "t (tracing stop)";

      if (strcmp (state, wanted) == 0)
	{
	  free (filename);
	  return;
	}

      if (sched_yield ())
	{
	  perror ("sched_yield()");
	  exit (EXIT_FAILURE);
	}

      i = clock_gettime (CLOCK_MONOTONIC, &now);
      assert (i == 0);
      assert (now.tv_sec >= start.tv_sec);
    }
  while (now.tv_sec - start.tv_sec < TIMEOUT);

  fprintf (stderr, "Timed out waiting for PID %lu \"%s\" (now it is \"%s\")!\n",
	   (unsigned long) process, wanted, state);
  exit (EXIT_FAILURE);
}

static volatile pid_t tracer = 0;
static pthread_t thread1, thread2;

static void
cleanup (void)
{
  printf ("Resuming GDB PID %lu.\n", (unsigned long) tracer);

  if (tracer)
    {
      int i;
      int tracer_save = tracer;

      tracer = 0;

      i = kill (tracer_save, SIGCONT);
      assert (i == 0);
    }
}

int
main (int argc, char **argv)
{
  int i;
  int standalone = 0;
  struct sigaction act;

  if (argc == 2 && strcmp (argv[1], "-s") == 0)
    standalone = 1;
  else
    assert (argc == 1);

  setbuf (stdout, NULL);

  timed_mutex_lock (&thread1_tid_mutex);
  timed_mutex_lock (&thread2_tid_mutex);

  timed_mutex_lock (&terminate_mutex);

  errno = 0;
  memset (&act, 0, sizeof (act));
  act.sa_sigaction = handler;
  act.sa_flags = SA_RESTART | SA_SIGINFO;
  i = sigemptyset (&act.sa_mask);
  assert_perror (errno);
  assert (i == 0);
  i = sigaction (SIGUSR1, &act, NULL);
  assert_perror (errno);
  assert (i == 0);
  i = sigaction (SIGUSR2, &act, NULL);
  assert_perror (errno);
  assert (i == 0);

  pthread_barrier_init (&threads_started_barrier, NULL, 3);

  i = pthread_create (&thread1, NULL, thread1_func, NULL);
  assert (i == 0);

  i = pthread_create (&thread2, NULL, thread2_func, NULL);
  assert (i == 0);

  if (!standalone)
    {
      tracer = proc_ulong ("/proc/self/status", "TracerPid:\t");
      if (tracer == 0)
	{
	  fprintf (stderr, "The testcase must be run by GDB!\n");
	  exit (EXIT_FAILURE);
	}
      if (tracer != getppid ())
	{
	  fprintf (stderr, "The testcase parent must be our GDB tracer!\n");
	  exit (EXIT_FAILURE);
	}
    }

  /* SIGCONT our debugger in the case of our crash as we would deadlock
     otherwise.  */

  atexit (cleanup);

  /* Wait until all threads are seen running.  On Linux (at least),
     new threads start stopped, and the debugger must resume them.
     Need to wait for that before stopping GDB.  */
  pthread_barrier_wait (&threads_started_barrier);

  printf ("Stopping GDB PID %lu.\n", (unsigned long) tracer);

  if (tracer)
    {
      i = kill (tracer, SIGSTOP);
      assert (i == 0);
      state_wait (tracer, "T (stopped)");
    }

  /* Threads are now waiting at timed_mutex_lock (thread1_tid_mutex)
     and so they could not trigger the signals before GDB is unstopped
     later.  Threads get resumed by the pthread_cond_wait below.  Use
     `while' loops for protection against spurious pthread_cond_wait
     wakeups.  */

  printf ("Waiting till the threads initialize their TIDs.\n");

  while (thread1_tid == 0)
    {
      i = pthread_cond_wait (&thread1_tid_cond, &thread1_tid_mutex);
      assert (i == 0);
    }

  while (thread2_tid == 0)
    {
      i = pthread_cond_wait (&thread2_tid_cond, &thread2_tid_mutex);
      assert (i == 0);
    }

  printf ("Thread 1 TID = %lu, thread 2 TID = %lu, PID = %lu.\n",
	  (unsigned long) thread1_tid, (unsigned long) thread2_tid,
	  (unsigned long) getpid ());

  errno = 0;
  i = tgkill (getpid (), thread1_tid, SIGUSR1);
  assert_perror (errno);
  assert (i == 0);
  i = tgkill (getpid (), thread1_tid, SIGUSR2);
  assert_perror (errno);
  assert (i == 0);
  i = tgkill (getpid (), thread2_tid, SIGUSR1);
  assert_perror (errno);
  assert (i == 0);
  i = tgkill (getpid (), thread2_tid, SIGUSR2);
  assert_perror (errno);
  assert (i == 0);

  printf ("Waiting till the threads are trapped by the signals.\n");

  if (tracer)
    {
      /* s390x-unknown-linux-gnu will fail with "R (running)".  */

      state_wait (thread1_tid, "t (tracing stop)");

      state_wait (thread2_tid, "t (tracing stop)");
    }

  cleanup ();

  printf ("Joining the threads.\n");

  i = pthread_mutex_unlock (&terminate_mutex);
  assert (i == 0);

  i = pthread_join (thread1, NULL);
  assert (i == 0);

  i = pthread_join (thread2, NULL);
  assert (i == 0);

  printf ("Exiting.\n");	/* break-at-exit */

  return EXIT_SUCCESS;
}
