/*
 * OS-Jackfruit stress helper
 * Authors:
 *   Mohammed Ibrahim Maqsood Khan (PES1UG24C578)
 *   Nitesh Harsur (PES1UG24CS584)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
	size_t chunk = 1024 * 1024;
	size_t count = 0;
	char **blocks = NULL;

	while (1) {
		char **tmp = realloc(blocks, (count + 1) * sizeof(*blocks));
		if (!tmp) {
			perror("realloc");
			return 1;
		}
		blocks = tmp;

		blocks[count] = malloc(chunk);
		if (!blocks[count]) {
			perror("malloc");
			return 1;
		}

		memset(blocks[count], 0xAB, chunk);
		count++;
		fprintf(stdout, "allocated=%zuMB\n", count);
		fflush(stdout);
		usleep(100 * 1000);
	}
}
