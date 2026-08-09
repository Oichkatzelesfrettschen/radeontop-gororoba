/*
    Copyright (C) 2012 Lauri Kasanen
    Copyright (C) 2018 Genesis Cloud Ltd.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, version 3 of the License.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "radeontop.h"
#include <getopt.h>
#include <errno.h>
#include <limits.h>
#include <string.h>

// A signal handler may touch only a volatile sig_atomic_t, and both output
// modes poll terminate_requested, so an interrupt reaches the same orderly shutdown that a
// line limit or a UI quit does: request stop, join, then unmap.
volatile sig_atomic_t terminate_requested = 0;

static void on_terminate(int signal_number) {
	(void) signal_number;
	terminate_requested = 1;
}

// A partially initialized struct sigaction leaves sa_mask and sa_flags
// indeterminate, so the handler would run under an unspecified signal mask.
static void install_terminate_handler(void) {
	struct sigaction action;

	memset(&action, 0, sizeof(action));
	action.sa_handler = on_terminate;
	sigemptyset(&action.sa_mask);
	action.sa_flags = 0;

	if (sigaction(SIGTERM, &action, NULL) || sigaction(SIGINT, &action, NULL))
		die(_("Failed to install the termination handlers"));
}

__attribute__((noreturn)) void die(const char * const why) {
	fprintf(stderr, "%s\n", why);
	exit(1);
}

// atoi and a bare strtoul report no error: a non-numeric argument becomes 0 and
// a negative one wraps through the unsigned option variables, so -t 0 reaches a
// modulo by zero in the collector, -t -1 reaches a 4-billion-sample window, and
// -b xyz selects bus 0 instead of naming the typo.  Accept a whole argument in
// the given base inside [lo, hi] and reject everything else.
static unsigned int parse_count(const char * const arg, const char * const what,
				unsigned long lo, unsigned long hi, int base) {
	char *end = NULL;
	unsigned long value;

	errno = 0;
	value = strtoul(arg, &end, base);

	if (errno || !end || end == arg || *end || strchr(arg, '-') ||
		value < lo || value > hi) {
		if (base == 16)
			fprintf(stderr, _("Invalid %s '%s': expected hexadecimal %lx to %lx\n"),
				what, arg, lo, hi);
		else
			fprintf(stderr, _("Invalid %s '%s': expected %lu to %lu\n"),
				what, arg, lo, hi);
		exit(1);
	}

	return (unsigned int) value;
}

static void version(void) {
	printf("RadeonTop %s\n", VERSION);
	exit(0);
}

// A successful informational request exits 0 on stdout and a rejected option
// exits nonzero on stderr, so a caller distinguishes the two by status and a
// pipeline keeps the diagnostic out of the data stream.  Upstream radeontop
// routes both through die() and exits 1 either way.
static void help(const char * const me, const unsigned int ticks,
		const unsigned int dumpinterval, const int status) {
	FILE * const out = status ? stderr : stdout;

	fprintf(out, _("\n\tRadeonTop for R600 and later, plus R300-class integrated RS4xx GPUs.\n\n"
		"\tUsage: %s [-cThmv] [--dither-seed seed] [-b bus] [-d file] [-i seconds] [-l limit] [-p device] [-t ticks]\n\n"
		"-b --bus 3		Pick card from the selected PCI bus (hexadecimal)\n"
		"-c --color		Enable colors\n"
		"-d --dump file		Dump data to the named file, - for stdout\n"
		"-i --dump-interval 1	Number of seconds between dumps (default %u)\n"
		"-l --limit 3		Quit after dumping N lines, default forever\n"
		"-m --mem		Force the validated PCI sysfs resourceN MMIO path\n"
		"-p --path device	Open DRM device node by path\n"
		"-t --ticks 50		Samples per second (default %u)\n"
		"-T --transparency	Enable transparency\n"
		"   --dither-seed 7	Offset each sample inside its own slot, seeded by N,\n"
		"			so a workload periodic at a harmonic of the sample\n"
		"			rate meets a moving phase.  Omit for the exact grid.\n"
		"\n"
		"-h --help		Show usage help\n"
		"-v --version		Show the version\n"),
		me, dumpinterval, ticks);
	exit(status);
}

// A setuid-root deployment can supply MMIO register access, so a privilege
// transition that silently fails leaves the wrong credentials in force for
// everything after it.  glibc marks the setuid family
// warn_unused_result under _FORTIFY_SOURCE, and each call site checks both
// the return value and the resulting effective id, because the id is the
// property that matters.
int main(int argc, char **argv) {
	// Temporarily drop privileges to do option parsing, etc.
	if (privileges_drop_effective())
		die(_("Failed to drop effective privileges"));

	const unsigned int default_ticks = 120;
	const unsigned int default_dumpinterval = 1;

	unsigned int ticks = default_ticks;
	unsigned char color = 0;
	unsigned char transparency = 0;
	short bus = -1;
	unsigned char forcemem = 0;
	unsigned int limit = 0;
	char *dump = NULL;
	unsigned int dumpinterval = default_dumpinterval;
	const char *path = NULL;
	// Zero keeps every sample on its exact grid point, which is the default and
	// what makes two runs of one workload directly comparable.
	uint64_t dither_seed = 0;
	struct radeon_device_identity identity;

	// Translations
#ifdef ENABLE_NLS
	setlocale(LC_ALL, "");
	bindtextdomain("radeontop", "/usr/share/locale");
	textdomain("radeontop");
#endif

	// A long-only option takes a value outside the character range, so getopt_long
	// returns it without colliding with a short option letter.
	enum {
		OPT_DITHER_SEED = 0x100,
		INFORMATION_NONE,
		INFORMATION_HELP,
		INFORMATION_VERSION
	};
	int information_request = INFORMATION_NONE;

	// opts
	const struct option opts[] = {
		{"dither-seed", required_argument, NULL, OPT_DITHER_SEED},
		{"bus", required_argument, NULL, 'b'},
		{"color", no_argument, NULL, 'c'},
		{"dump", required_argument, NULL, 'd'},
		{"dump-interval", required_argument, NULL, 'i'},
		{"help", no_argument, NULL, 'h'},
		{"limit", required_argument, NULL, 'l'},
		{"mem", no_argument, NULL, 'm'},
		{"path", required_argument, NULL, 'p'},
		{"ticks", required_argument, NULL, 't'},
		{"transparency", no_argument, NULL, 'T'},
		{"version", no_argument, NULL, 'v'},
		{NULL, 0, NULL, 0}
	};

	while (1) {
		// A leading plus keeps argv in its original order and stops at the first
		// operand.  The CLI accepts no operands, so the post-parse check rejects
		// both an operand before options and one after options.
		int c = getopt_long(argc, argv, "+b:cTd:hi:l:mp:t:v", opts, NULL);
		if (c == -1) break;

		switch(c) {
			case 'h':
				if (information_request == INFORMATION_NONE)
					information_request = INFORMATION_HELP;
			break;
			case '?':
				// getopt_long has already named the offending option
				// on stderr; the usage text follows on the same stream.
				help(argv[0], default_ticks, default_dumpinterval, 1);
			break;
			case 't':
				ticks = parse_count(optarg, _("tick count"), 1, 1000000, 10);
			break;
			case 'T':
				transparency = 1;
			break;
			case 'c':
				color = 1;
			break;
			case 'm':
				forcemem = 1;
			break;
			case 'b':
				// A PCI bus number spans one byte, and bus stays at
				// its -1 unset sentinel when the option is absent.
				bus = (short) parse_count(optarg, _("bus number"), 0, 0xff, 16);
			break;
			case 'v':
				if (information_request == INFORMATION_NONE)
					information_request = INFORMATION_VERSION;
			break;
			case 'l':
				limit = parse_count(optarg, _("dump limit"), 0, UINT_MAX, 10);
			break;
			case 'd':
				dump = optarg;
			break;
			case 'i':
				dumpinterval = parse_count(optarg, _("dump interval"), 1, 86400, 10);
			break;
			case 'p':
				path = optarg;
			break;
			case OPT_DITHER_SEED:
				// Zero is the off state and the flag's absence is how to
				// select it, so an explicit zero is a rejected value
				// rather than a second spelling of the default.
				dither_seed = parse_count(optarg, _("dither seed"), 1, UINT_MAX, 10);
			break;
		}
	}

	if (optind < argc) {
		fprintf(stderr, _("Unexpected operand '%s'\n"), argv[optind]);
		help(argv[0], default_ticks, default_dumpinterval, 1);
	}
	if (information_request == INFORMATION_HELP)
		help(argv[0], default_ticks, default_dumpinterval, 0);
	if (information_request == INFORMATION_VERSION)
		version();

	// DRM discovery and authentication run unprivileged.  init_pci raises the
	// saved effective identity only around a validated direct BAR mapping.
	init_pci(path, &bus, &identity, forcemem);

	// Drop permanently before the collector, the UI, and the dump writer run.
	// setresuid moves the real, effective, and saved ids together, so a failure
	// during the permanent drop would leave those components able to return
	// to root.
	if (privileges_drop_permanently())
		die(_("Failed to drop privileges"));

	const int family = identity.family;
	if (family == UNKNOWN_CHIP)
		die(_("Unsupported Radeon PCI ID: no validated family model"));

	const char * const cardname = family_str[family];

	initbits(family);

	// The collector reads through the mapped BAR and the DRM handles that
	// cleanup() releases, so it is started after the backend exists and
	// stopped and joined before the backend goes away.  A detached worker
	// could enter a read while cleanup() unmapped the window under it.
	struct collector_monotonic_clock clock;
	if (collector_monotonic_clock_init(&clock))
		die(_("Failed to initialize the collector clock"));

	const struct collector_config config = { ticks, dumpinterval, dither_seed };
	const struct collector_backend backend = collector_backend_from_device();
	const struct engine_masks masks = collector_masks_from_bits();
	const struct collector_clock clock_ops = collector_monotonic_clock_ops(&clock);
	struct radeontop_capture_metadata capture_metadata;
	const struct radeontop_capture_metadata *capture = NULL;
	struct collector collector;

	if (dump) {
		const struct radeontop_build_identity build_identity = {
			VERSION,
			RADEONTOP_SOURCE_COMMIT,
			RADEONTOP_SOURCE_STATE,
			RADEONTOP_SOURCE_SHA256,
			RADEONTOP_SOURCE_MANIFEST,
			RADEONTOP_BUILD_MANIFEST_SHA256,
			RADEONTOP_BUILD_MANIFEST
		};

		if (radeontop_capture_metadata_init(&capture_metadata, &build_identity,
				cardname,
				argc, argv, &identity, &config, vramsize, gttsize,
				sclk_max, mclk_max))
			die(_("Failed to initialize capture provenance"));
		capture = &capture_metadata;
	}

	if (collector_init(&collector, &config, &backend, &masks, &clock_ops))
		die(_("Failed to initialize the collector"));

	install_terminate_handler();

	if (collector_start(&collector))
		die(_("Failed to start the collector thread"));

	int status;

	if (dump)
		status = dumpdata(&collector, &masks, capture, dump, limit, bus);
	else
		status = present(&collector, &masks, cardname, color, transparency, bus);

	collector_request_stop(&collector);
	if (collector_join(&collector)) {
		// A failed join leaves worker termination unproven.  Process exit owns
		// cleanup in the join-failure branch; unmapping or destroying synchronization state
		// could race a live hardware read.
		fprintf(stderr, _("Failed to join the collector thread.\n"));
		return 1;
	}
	if (collector_destroy(&collector))
		status = 1;
	collector_monotonic_clock_destroy(&clock);

	cleanup();
	return status;
}
