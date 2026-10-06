#define _DEFAULT_SOURCE

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "core/stge.h"

static int64_t tick;

static void usage(void)
{
	fprintf(stderr, "usage: stge -game <dir> [-check]\n");
}

int main(int argc, char **argv)
{
	bool check_only = false;

	if (argc < 3 || argc > 4 || strcmp(argv[1], "-game") != 0)
	{
		usage();
		return 1;
	}

	if (argc == 4)
	{
		if (strcmp(argv[3], "-check") != 0)
		{
			usage();
			return 1;
		}

		check_only = true;
	}

	if (!stge_init())
	{
		fprintf(stderr, "stge: could not init\n");
		return 1;
	}

	if (chdir(argv[2]) != 0)
	{
		fprintf(stderr, "stge: could not open game %s\n", argv[2]);
		stge_shutdown();
		return 1;
	}

	if (!stge_load("main.stge"))
	{
		int code = stge_parse_failed() ? 2 : 1;

		if (code == 1)
			fprintf(stderr, "stge: could not load main.stge\n");
		stge_shutdown();
		return code;
	}

	if (!check_only)
		stge_run(&tick);

	{
		int code = stge_exit_code();

		stge_shutdown();
		return code;
	}
}
