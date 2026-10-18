/*
 * Fork-free clock sampler for the GNU/Hurd SMP + vioif0 stress test.
 *
 * The shell sampler that came first was measuring its own fork/exec overhead:
 * every sample cost a `date` and a `cat`, which under load is tens of
 * milliseconds, and that shows up as a rate error in the trace. This program
 * reads the clock in a tight loop instead, so the sampling cost is
 * microseconds and the trace can be read at face value.
 *
 * For each sample it records three independent clocks:
 *
 *   monotonic  CLOCK_MONOTONIC, the C library's monotonic clock;
 *   realtime   CLOCK_REALTIME, the wall clock;
 *   uptime     the first field of /proc/uptime, the kernel's own counter.
 *
 * The kernel's counter is the one that matters: if the 100 Hz clock froze,
 * `uptime` would stop advancing while the other two would not.
 *
 * Usage: clock-sampler <seconds> <outfile>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
	double seconds = 60.0;
	FILE *out;
	struct timespec start;
	FILE *up;

	if (argc < 3) {
		fprintf(stderr, "usage: %s <seconds> <outfile>\n", argv[0]);
		return 2;
	}

	seconds = strtod(argv[1], NULL);
	out = fopen(argv[2], "w");
	if (out == NULL) {
		perror(argv[2]);
		return 1;
	}

	up = fopen("/proc/uptime", "r");
	if (up == NULL) {
		perror("/proc/uptime");
		fclose(out);
		return 1;
	}

	clock_gettime(CLOCK_MONOTONIC, &start);

	for (;;) {
		struct timespec mono;
		struct timespec real;
		char line[128];
		double uptime = 0.0;
		double elapsed;
		char *end;

		clock_gettime(CLOCK_MONOTONIC, &mono);
		clock_gettime(CLOCK_REALTIME, &real);

		if (fgets(line, sizeof(line), up) == NULL) {
			/* The procfs entry can be re-read after each use. */
			rewind(up);
			if (fgets(line, sizeof(line), up) == NULL)
				break;
		}
		uptime = strtod(line, &end);

		fprintf(out, "%.3f %.3f %.3f\n", (double)mono.tv_sec +
			(double)mono.tv_nsec / 1e9,
			(double)real.tv_sec + (double)real.tv_nsec / 1e9,
			uptime);
		fflush(out);

		elapsed = (double)(mono.tv_sec - start.tv_sec) +
			  (double)(mono.tv_nsec - start.tv_nsec) / 1e9;
		if (elapsed >= seconds)
			break;

		usleep(100000); /* 10 ms */
	}

	fclose(up);
	fclose(out);
	return 0;
}
