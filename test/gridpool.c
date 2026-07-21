#include <stdio.h>
#include <stdlib.h>

#include "gridpool_adapter.h"

static void expect(bool condition, const char *message)
{
	if (!condition) {
		fprintf(stderr, "GridPool test failed: %s\n", message);
		exit(EXIT_FAILURE);
	}
}

int main(void)
{
	expect(gridpool_password_enabled("USE_GRIDPOOL_SPLIT"), "exact flag");
	expect(gridpool_password_enabled("x,USE_GRIDPOOL_SPLIT,d=1024"), "comma token");
	expect(gridpool_password_enabled("x;use_gridpool_split"), "case-insensitive token");
	expect(!gridpool_password_enabled("NOT_USE_GRIDPOOL_SPLIT"), "no substring match");
	expect(!gridpool_password_enabled("USE_GRIDPOOL_SPLIT_EXTRA"), "no prefix match");
	expect(!gridpool_password_enabled(NULL), "null password");
	return EXIT_SUCCESS;
}

