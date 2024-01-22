#include <assert.h>
#include <unistd.h>
#include <sys/mman.h>

#include <sepol/debug.h>
#include <sepol/kernel_to_cil.h>
#include <sepol/kernel_to_conf.h>
#include <sepol/module_to_cil.h>
#include <sepol/policydb/policydb.h>
#include <sepol/policydb/hierarchy.h>
#include <sepol/policydb/expand.h>
#include <sepol/policydb/link.h>

#include "module_compiler.h"
#include "queue.h"

extern int policydb_validate(sepol_handle_t *handle, const policydb_t *p);
extern int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

extern int mlspol;
extern policydb_t *policydbp;
extern queue_t id_queue;
extern unsigned int policydb_errors;

extern int yynerrs;
extern FILE *yyin;
extern void init_parser(int);
extern int yyparse(void);
extern void yyrestart(FILE *);
extern int yylex_destroy(void);
extern void set_source_file(const char *name);


// Set to 1 for verbose libsepol logging
#define VERBOSE 0

static ssize_t full_write(int fd, const void *buf, size_t count)
{
	ssize_t written = 0;

	while (count > 0) {
		ssize_t ret = write(fd, buf, count);
		if (ret < 0) {
			if (errno == EINTR)
				continue;

			return ret;
		}

		if (ret == 0)
			break;

		written += ret;
		buf = (const unsigned char *)buf + (size_t)ret;
		count -= (size_t)ret;
	}

	return written;
}

static int read_source_policy(policydb_t *p, const uint8_t *data, size_t size)
{
	int fd, rc;
	ssize_t wr;

	fd = memfd_create("fuzz-input", MFD_CLOEXEC);
	if (fd < 0)
		return -1;

	wr = full_write(fd, data, size);
	if (wr < 0 || (size_t)wr != size) {
		close(fd);
		return -1;
	}

	fsync(fd);

	yynerrs = 0;

	yyin = fdopen(fd, "r");
	if (!yyin) {
		close(fd);
		return -1;
	}

	rewind(yyin);

	set_source_file("fuzz-input");

	id_queue = queue_create();
	if (id_queue == NULL) {
		fclose(yyin);
		yylex_destroy();
		return -1;
	}

	policydbp = p;
	mlspol = p->mls;

	init_parser(1);

	rc = yyparse();
	// TODO: drop global variable policydb_errors if proven to be redundant
	assert(rc || !policydb_errors);
	if (rc || policydb_errors) {
		queue_destroy(id_queue);
		fclose(yyin);
		yylex_destroy();
		return -1;
	}

	rewind(yyin);
	init_parser(2);
	set_source_file("fuzz-input");
	yyrestart(yyin);

	rc = yyparse();
	assert(rc || !policydb_errors);
	if (rc || policydb_errors) {
		queue_destroy(id_queue);
		fclose(yyin);
		yylex_destroy();
		return -1;
	}

	queue_destroy(id_queue);
	fclose(yyin);
	yylex_destroy();

	return 0;
}

static int check_level(hashtab_key_t key, hashtab_datum_t datum, void *arg __attribute__ ((unused)))
{
	const level_datum_t *levdatum = (level_datum_t *) datum;

	// TODO: drop member defined if proven to be always set
	if (!levdatum->isalias && !levdatum->defined) {
		fprintf(stderr,
			"Error:  sensitivity %s was not used in a level definition!\n",
			key);
		abort();
	}

	return 0;
}

static int write_binary_policy(FILE *outfp, policydb_t *p)
{
	struct policy_file pf;

	policy_file_init(&pf);
	pf.type = PF_USE_STDIO;
	pf.fp = outfp;
	return policydb_write(p, &pf);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	policydb_t parsepolicydb = {};
	policydb_t kernpolicydb = {};
	policydb_t *finalpolicydb;
	sidtab_t sidtab = {};
	FILE *devnull = NULL;
	int mls, policyvers;

	sepol_debug(VERBOSE);

	/* Take the first byte whether to parse as MLS policy
	* and the second byte as policy version. */
	if (size < 2)
		return 0;
	switch (data[0]) {
	case '0':
		mls = 0;
		break;
	case '1':
		mls = 1;
		break;
	default:
		return 0;
	}
	static_assert(0x7F - 'A' >= POLICYDB_VERSION_MAX, "Max policy version should be representable");
	policyvers = data[1] - 'A';
	if (policyvers < POLICYDB_VERSION_MIN || policyvers > POLICYDB_VERSION_MAX)
		return 0;
	data += 2;
	size -= 2;

	if (policydb_init(&parsepolicydb))
		goto exit;

	parsepolicydb.policy_type = POLICY_BASE;
	parsepolicydb.mls = mls;
	parsepolicydb.handle_unknown = DENY_UNKNOWN;
	policydb_set_target_platform(&parsepolicydb, SEPOL_TARGET_SELINUX);

	if (read_source_policy(&parsepolicydb, data, size))
		goto exit;

	(void) hashtab_map(parsepolicydb.p_levels.table, check_level, NULL);

	if (parsepolicydb.policy_type == POLICY_BASE) {
		if (link_modules(NULL, &parsepolicydb, NULL, 0, VERBOSE))
			goto exit;

		if (policydb_init(&kernpolicydb))
			goto exit;

		if (expand_module(NULL, &parsepolicydb, &kernpolicydb, VERBOSE, /*check_assertions=*/0))
			goto exit;

		(void) check_assertions(NULL, &kernpolicydb, kernpolicydb.global->branch_list->avrules);
		(void) hierarchy_check_constraints(NULL, &kernpolicydb);

		kernpolicydb.policyvers = policyvers;

		assert(kernpolicydb.policy_type    == POLICY_KERN);
		assert(kernpolicydb.handle_unknown == SEPOL_DENY_UNKNOWN);
		assert(kernpolicydb.mls            == mls);

		finalpolicydb = &kernpolicydb;
	} else {
		assert(parsepolicydb.policy_type    == POLICY_MOD);
		assert(parsepolicydb.handle_unknown == SEPOL_DENY_UNKNOWN);
		assert(parsepolicydb.mls            == mls);

		finalpolicydb = &parsepolicydb;
	}

	if (policydb_load_isids(finalpolicydb, &sidtab))
		goto exit;

	if (finalpolicydb->policy_type == POLICY_KERN && policydb_optimize(finalpolicydb))
		goto exit;

	if (policydb_sort_ocontexts(finalpolicydb))
		goto exit;

	if (policydb_validate(NULL, finalpolicydb))
		/* never generate an invalid policy */
		abort();

	devnull = fopen("/dev/null", "we");
	if (devnull == NULL)
		goto exit;

	if (write_binary_policy(devnull, finalpolicydb))
		abort();

	if (finalpolicydb->policy_type == POLICY_KERN && sepol_kernel_policydb_to_conf(devnull, finalpolicydb))
		abort();

	if (finalpolicydb->policy_type == POLICY_KERN && sepol_kernel_policydb_to_cil(devnull, finalpolicydb))
		abort();

	if (finalpolicydb->policy_type == POLICY_MOD && sepol_module_policydb_to_cil(devnull, finalpolicydb, /*linked=*/0))
		abort();

exit:
	if (devnull != NULL)
		fclose(devnull);

	sepol_sidtab_destroy(&sidtab);
	policydb_destroy(&kernpolicydb);
	policydb_destroy(&parsepolicydb);

	id_queue = NULL;
	policydbp = NULL;
	module_compiler_reset();

	/* Non-zero return values are reserved for future use. */
	return 0;
}
