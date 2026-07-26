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
// modes poll this one, so an interrupt reaches the same orderly shutdown that a
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

	fprintf(out, _("\n\tRadeonTop for R600 and above, plus R300-class IGPs (RS4xx, use -m).\n\n"
		"\tUsage: %s [-chmv] [-b bus] [-d file] [-i seconds] [-l limit] [-p device] [-t ticks]\n\n"
		"-b --bus 3		Pick card from this PCI bus (hexadecimal)\n"
		"-c --color		Enable colors\n"
		"-d --dump file		Dump data to this file, - for stdout\n"
		"-i --dump-interval 1	Number of seconds between dumps (default %u)\n"
		"-l --limit 3		Quit after dumping N lines, default forever\n"
		"-m --mem		Force the PCI sysfs resourceN MMIO path, for the proprietary driver\n"
		"-p --path device	Open DRM device node by path\n"
		"-t --ticks 50		Samples per second (default %u)\n"
		"-T --transparency	Enable transparency\n"
		"\n"
		"-h --help		Show this help\n"
		"-v --version		Show the version\n"),
		me, dumpinterval, ticks);
	exit(status);
}

// The binary runs setuid root on installations that need MMIO register access,
// so a privilege transition that silently fails leaves the wrong credentials in
// force for everything after it.  glibc marks the setuid family
// warn_unused_result under _FORTIFY_SOURCE, and each call site here checks both
// the return value and the resulting effective id, because the id is the
// property that matters.
static void drop_euid(void) {
	const uid_t uid = getuid();

	if (seteuid(uid) || geteuid() != uid)
		die(_("Failed to drop effective privileges"));
}

int main(int argc, char **argv) {
	// Temporarily drop privileges to do option parsing, etc.
	drop_euid();

	const unsigned int default_ticks = 120;
	const unsigned int default_dumpinterval = 1;

	unsigned int ticks = default_ticks;
	unsigned char color = 0;
	unsigned char transparency = 0;
	short bus = -1;
	unsigned char forcemem = 0;
	unsigned int device_id = 0;
	unsigned int limit = 0;
	char *dump = NULL;
	unsigned int dumpinterval = default_dumpinterval;
	const char *path = NULL;

	// Translations
#ifdef ENABLE_NLS
	setlocale(LC_ALL, "");
	bindtextdomain("radeontop", "/usr/share/locale");
	textdomain("radeontop");
#endif

	// opts
	const struct option opts[] = {
		{"bus", 1, 0, 'b'},
		{"color", 0, 0, 'c'},
		{"dump", 1, 0, 'd'},
		{"dump-interval", 1, 0, 'i'},
		{"help", 0, 0, 'h'},
		{"limit", 1, 0, 'l'},
		{"mem", 0, 0, 'm'},
		{"path", 1, 0, 'p'},
		{"ticks", 1, 0, 't'},
		{"transparency", 0, 0, 'T'},
		{"version", 0, 0, 'v'},
		{0, 0, 0, 0}
	};

	while (1) {
		int c = getopt_long(argc, argv, "b:cTd:hi:l:mp:t:v", opts, NULL);
		if (c == -1) break;

		switch(c) {
			case 'h':
				help(argv[0], default_ticks, default_dumpinterval, 0);
			break;
			case '?':
				// getopt_long has already named the offending option
				// on stderr; the usage text follows it there.
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
				version();
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
		}
	}

	// init (regain privileges for bus initialization and ultimately drop them afterwards)
	// An unprivileged run cannot regain root and reports EPERM, and the DRM
	// and xcb paths still work without it, so init_pci reports whichever
	// access it cannot obtain.  Any other failure is unexpected.
	if (seteuid(0) && errno != EPERM)
		die(_("Failed to regain privileges"));

	init_pci(path, &bus, &device_id, forcemem);
	// after init_pci we can assume that bus/device_id exists (otherwise it would die())

	// Drop permanently before the collector, the UI, and the dump writer run.
	// setuid moves the saved id too, so a failure here would leave every one
	// of them able to return to root.
	if (setuid(getuid()) || geteuid() != getuid())
		die(_("Failed to drop privileges"));

	const int family = getfamily(device_id);
	if (!family)
		fprintf(stderr, _("Unknown Radeon card. <= R500 won't work, new cards might.\n"));

	const char * const cardname = family_str[family];

	initbits(family);

	// The collector reads through the mapped BAR and the DRM handles that
	// cleanup() releases, so it is started after the backend exists and
	// stopped and joined before the backend goes away.  A detached worker
	// could enter a read while cleanup() unmapped the window under it.
	struct collector_monotonic_clock clock;
	if (collector_monotonic_clock_init(&clock))
		die(_("Failed to initialize the collector clock"));

	const struct collector_config config = { ticks, dumpinterval };
	const struct collector_backend backend = collector_backend_from_device();
	const struct engine_masks masks = collector_masks_from_bits();
	const struct collector_clock clock_ops = collector_monotonic_clock_ops(&clock);
	struct collector collector;

	if (collector_init(&collector, &config, &backend, &masks, &clock_ops))
		die(_("Failed to initialize the collector"));

	install_terminate_handler();

	if (collector_start(&collector))
		die(_("Failed to start the collector thread"));

	int status;

	if (dump)
		status = dumpdata(&collector, &masks, dump, limit, bus);
	else
		status = present(&collector, &masks, cardname, color, transparency, bus);

	collector_request_stop(&collector);
	if (collector_join(&collector))
		status = 1;
	collector_destroy(&collector);
	collector_monotonic_clock_destroy(&clock);

	cleanup();
	return status;
}
