#include <tools/ccanlint/ccanlint.h>
#include <tools/tools.h>
#include <ccan/talloc/talloc.h>
#include <ccan/str/str.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <err.h>
#include <string.h>
#include <ctype.h>
#include "build.h"

static const char *can_build(struct manifest *m)
{
	if (safe_mode)
		return "Safe mode enabled";
	return NULL;
}

static bool expect_obj_file(struct manifest *m)
{
	/* If it has C files, we expect an object file built from them. */
	return !list_empty(&m->c_files);
}

static char *build_subdir_objs(struct manifest *m)
{
	struct ccan_file *i;

	list_for_each(&m->c_files, i, list) {
		char *fullfile = talloc_asprintf(m, "%s/%s", m->dir, i->name);
		char *output;

		i->compiled = maybe_temp_file(m, "", false, fullfile);
		if (!compile_object(m, fullfile, ccan_dir, "", i->compiled,
				    &output)) {
			talloc_free(i->compiled);
			i->compiled = NULL;
			return talloc_asprintf(m,
					       "Dependency %s"
					       " did not build:\n%s",
					       m->basename, output);
		}
	}
	return NULL;
}

char *build_submodule(struct manifest *m)
{
	char *errstr;
	struct stat st;

	if (m->compiled)
		return NULL;

	if (!expect_obj_file(m))
		return NULL;

	m->compiled = talloc_asprintf(m, "%s.o", m->dir);
	if (stat(m->compiled, &st) == 0)
		return NULL;

	if (verbose >= 2)
		printf("  Building dependency %s\n", m->dir);

	errstr = build_subdir_objs(m);
	if (errstr)
		return errstr;

	m->compiled = build_module(m, false, &errstr);
	if (!m->compiled)
		return errstr;
	return NULL;
}

static void check_depends_built(struct manifest *m,
				bool keep,
				unsigned int *timeleft, struct score *score)
{
	struct manifest *i;

	list_for_each(&m->deps, i, list) {
		char *errstr = build_submodule(i);

		if (errstr) {
			score->error = talloc_asprintf(score,
						       "Dependency %s"
						       " did not build:\n%s",
						       i->basename, errstr);
			return;
		}
	}

	score->pass = true;
	score->score = score->total;
}

struct ccanlint depends_build = {
	.key = "depends_build",
	.name = "Module's CCAN dependencies can be found or built",
	.check = check_depends_built,
	.can_run = can_build,
	.needs = "depends_exist"
};

REGISTER_TEST(depends_build);
