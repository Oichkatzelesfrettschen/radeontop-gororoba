/*
	Copyright (C) 2012 Lauri Kasanen

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

#include <stdbool.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>

static uid_t test_getuid(void);
static uid_t test_geteuid(void);
static int test_seteuid(uid_t effective_uid);
static int test_setresuid(uid_t real_uid, uid_t effective_uid,
	uid_t saved_uid);
static int test_getresuid(uid_t *real_uid, uid_t *effective_uid,
	uid_t *saved_uid);

#define getuid test_getuid
#define geteuid test_geteuid
#define seteuid test_seteuid
#define setresuid test_setresuid
#define getresuid test_getresuid
#include "../privileges.c"
#undef getresuid
#undef setresuid
#undef seteuid
#undef geteuid
#undef getuid

static unsigned int checks;
static unsigned int failures;
static uid_t current_real_uid;
static uid_t current_effective_uid;
static uid_t current_saved_uid;
static bool seteuid_fails;
static bool seteuid_ignores_request;
static bool setresuid_fails;
static bool getresuid_fails;
static bool getresuid_reports_mismatch;
static unsigned int seteuid_calls;
static unsigned int setresuid_calls;
static unsigned int getresuid_calls;

#define CHECK(condition) do { \
	checks++; \
	if (!(condition)) { \
		fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
		failures++; \
	} \
} while (0)

static void reset_credentials(void) {
	current_real_uid = 1000;
	current_effective_uid = 0;
	current_saved_uid = 0;
	seteuid_fails = false;
	seteuid_ignores_request = false;
	setresuid_fails = false;
	getresuid_fails = false;
	getresuid_reports_mismatch = false;
	seteuid_calls = 0;
	setresuid_calls = 0;
	getresuid_calls = 0;
}

static uid_t test_getuid(void) {
	return current_real_uid;
}

static uid_t test_geteuid(void) {
	return current_effective_uid;
}

static int test_seteuid(uid_t effective_uid) {
	seteuid_calls++;
	if (seteuid_fails)
		return -1;
	if (effective_uid != current_real_uid &&
		effective_uid != current_saved_uid)
		return -1;
	if (!seteuid_ignores_request)
		current_effective_uid = effective_uid;
	return 0;
}

static int test_setresuid(uid_t real_uid, uid_t effective_uid,
		uid_t saved_uid) {
	setresuid_calls++;
	if (setresuid_fails)
		return -1;
	current_real_uid = real_uid;
	current_effective_uid = effective_uid;
	current_saved_uid = saved_uid;
	return 0;
}

static int test_getresuid(uid_t *real_uid, uid_t *effective_uid,
		uid_t *saved_uid) {
	getresuid_calls++;
	if (getresuid_fails)
		return -1;
	*real_uid = current_real_uid;
	*effective_uid = current_effective_uid;
	*saved_uid = getresuid_reports_mismatch ? 0 : current_saved_uid;
	return 0;
}

static void check_effective_transitions(void) {
	reset_credentials();
	CHECK(privileges_drop_effective() == 0);
	CHECK(current_effective_uid == current_real_uid);
	CHECK(seteuid_calls == 1);
	CHECK(privileges_raise_effective() == 0);
	CHECK(current_effective_uid == 0);
	CHECK(seteuid_calls == 2);

	reset_credentials();
	seteuid_fails = true;
	CHECK(privileges_drop_effective() == -1);
	CHECK(current_effective_uid == 0);
	CHECK(privileges_raise_effective() == -1);

	reset_credentials();
	seteuid_ignores_request = true;
	CHECK(privileges_drop_effective() == -1);
	current_effective_uid = current_real_uid;
	CHECK(privileges_raise_effective() == -1);
}

static void check_permanent_transition(void) {
	reset_credentials();
	CHECK(privileges_drop_permanently() == 0);
	CHECK(current_real_uid == 1000);
	CHECK(current_effective_uid == 1000);
	CHECK(current_saved_uid == 1000);
	CHECK(setresuid_calls == 1);
	CHECK(getresuid_calls == 1);
	CHECK(privileges_raise_effective() == -1);

	reset_credentials();
	setresuid_fails = true;
	CHECK(privileges_drop_permanently() == -1);
	CHECK(getresuid_calls == 0);
	CHECK(current_saved_uid == 0);

	reset_credentials();
	getresuid_fails = true;
	CHECK(privileges_drop_permanently() == -1);
	CHECK(setresuid_calls == 1);
	CHECK(getresuid_calls == 1);

	reset_credentials();
	getresuid_reports_mismatch = true;
	CHECK(privileges_drop_permanently() == -1);
}

int main(void) {
	check_effective_transitions();
	check_permanent_transition();

	printf("privileges: %u checks, %u failed\n", checks, failures);
	return failures ? 1 : 0;
}
