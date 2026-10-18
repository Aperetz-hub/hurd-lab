/*
 * CPU burner for the GNU/Hurd SMP + vioif0 stress test.
 *
 * Runs a fixed kind of integer work until a wall-clock deadline, and reports
 * how much work it actually managed to do.  The iteration count is the point:
 * two of these running at the same time, one pinned to the master processor
 * set and one to the slave, show whether the second CPU really is running
 * and not merely present.
 *
 * Built and run inside the guest:
 *     gcc -O2 -o burn burn.c
 *     ./burn 60
 */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int
main(int argc, char **argv)
{
	unsigned long seconds = 60;
	unsigned long iterations = 0;
	volatile unsigned long acc = 0;
	struct timespec start;
	struct timespec now;
	double elapsed = 0.0;
	int k;

	if (argc > 1)
		seconds = strtoul(argv[1], NULL, 10);

	clock_gettime(CLOCK_MONOTONIC, &start);

	for (;;) {
		for (k = 0; k < 10000; k++) {
			acc = acc * 6364136223846793005UL + 1442695040888963407UL;
			iterations++;
		}

		clock_gettime(CLOCK_MONOTONIC, &now);
		elapsed = (double)(now.tv_sec - start.tv_sec) +
			  (double)(now.tv_nsec - start.tv_nsec) / 1e9;
		if (elapsed >= (double)seconds)
			break;
	}

	printf("burner: %lu iterations, checksum %lu, %.2f s\n", iterations,
	       acc, elapsed);
	return 0;
}
