/*
 * Usage: ./prog <filename> <n>
 * Runs for n seconds, overwriting <filename> every second with
 * "Process ran k out of n secs"
 * Output files always go in scratch (created if missing)
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

#define OUTPUT_DIR "scratch"

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <filename> <seconds>\n", argv[0]);
        return 1;
    }

    char *end;
    long n = strtol(argv[2], &end, 10);
    if (*end != '\0' || n < 0) {
        fprintf(stderr, "Error: '%s' is not a valid number of seconds\n", argv[2]);
        return 1;
    }

    // make the output dir, it's fine if it already exists
    if (mkdir(OUTPUT_DIR, 0755) == -1 && errno != EEXIST) {
        perror("Error creating " OUTPUT_DIR);
        return 1;
    }

    char path[4096];
    snprintf(path, sizeof(path), "%s/%s", OUTPUT_DIR, argv[1]);

    for (long k = 1; k <= n; k++) {
        sleep(1);

        FILE *fp = fopen(path, "w");
        if (fp == NULL) {
            perror("Error opening output file");
            return 1;
        }
        fprintf(fp, "Process ran %ld out of %ld secs\n", k, n);
        fclose(fp);
    }

    return 0;
}
