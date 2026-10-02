// SPDX-License-Identifier: GPL-2.0-only
/* glob() must unescape a directory without metacharacters. Hush expands a
 * quoted directory followed by a wildcard with each '-' escaped as "\-". */
#include <glob.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

int main(void)
{
	glob_t g;
	FILE *f;

	mkdir("/tmp/glob-test", 0755);
	f = fopen("/tmp/glob-test/entry", "w");
	if (!f)
		return 1;
	fclose(f);
	if (glob("/tmp/glob\\-test/*", 0, NULL, &g) || g.gl_pathc != 1 ||
	    strcmp(g.gl_pathv[0], "/tmp/glob-test/entry"))
		return 2;
	globfree(&g);
	puts("GLOB PASS");
	return 0;
}
