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

#include "capture.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static unsigned int checks;
static unsigned int failures;
static const char source_manifest_fixture[] = "source-fixture\n";
static const char build_manifest_fixture[] = "build-fixture\n";
static const char source_manifest_fixture_sha256[] =
	"1bb682ef52f9e73e62cfb0c2928482c99d13ed9048d499aee899d5b6a5f90d04";
static const char build_manifest_fixture_sha256[] =
	"5f706471124a2971edc01f26aa5fc35d0eef5c28e89395e1109d09121c94ccbd";

#define CHECK(condition) do { \
	checks++; \
	if (!(condition)) { \
		fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
		failures++; \
	} \
} while (0)

static bool read_stream(FILE *stream, char *buffer, size_t size) {
	long length;

	if (fflush(stream) || fseek(stream, 0, SEEK_END))
		return false;
	length = ftell(stream);
	if (length < 0 || (size_t) length >= size || fseek(stream, 0, SEEK_SET))
		return false;
	if (fread(buffer, 1, (size_t) length, stream) != (size_t) length)
		return false;
	buffer[length] = '\0';
	return true;
}

static void check_header(void) {
	char argument_with_control[] = { 'c', 't', 'l', 1, 'x', '\0' };
	char argument_with_bytes[] = {
		'b', 'y', 't', 'e', 0x7f, (char) 0x80, (char) 0xff, '\0'
	};
	char *argv[] = {
		"radeontop",
		"quote\"line\nslash\\end",
		argument_with_control,
		argument_with_bytes
	};
	struct radeontop_capture_metadata metadata;
	FILE *stream;
	char output[8192];
	unsigned int newlines = 0;

	memset(&metadata, 0, sizeof(metadata));
	memcpy(metadata.run_id, "11111111-2222-4333-8444-555555555555", 37);
	memcpy(metadata.boot_id, "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee", 37);
	snprintf(metadata.kernel_release, sizeof(metadata.kernel_release), "test-kernel");
	snprintf(metadata.source_version, sizeof(metadata.source_version), "v1.4-test");
	snprintf(metadata.source_commit, sizeof(metadata.source_commit),
		"0123456789abcdef0123456789abcdef01234567");
	snprintf(metadata.source_state, sizeof(metadata.source_state), "clean");
	snprintf(metadata.source_sha256, sizeof(metadata.source_sha256), "%s",
		source_manifest_fixture_sha256);
	snprintf(metadata.build_manifest_sha256,
		sizeof(metadata.build_manifest_sha256), "%s",
		build_manifest_fixture_sha256);
	metadata.source_manifest = source_manifest_fixture;
	metadata.build_manifest = build_manifest_fixture;
	snprintf(metadata.family_name, sizeof(metadata.family_name), "RS480");
	metadata.started_realtime.tv_sec = 10;
	metadata.started_realtime.tv_nsec = 20;
	metadata.started_monotonic.tv_sec = 30;
	metadata.started_monotonic.tv_nsec = 40;
	metadata.argc = 4;
	metadata.argv = argv;
	radeon_device_identity_init(&metadata.identity);
	metadata.identity.pci_address_valid = true;
	metadata.identity.domain = 0;
	metadata.identity.bus = 1;
	metadata.identity.device = 5;
	metadata.identity.function = 0;
	metadata.identity.vendor_id = 0x1002;
	metadata.identity.device_id = 0x5974;
	metadata.identity.family = RS480;
	metadata.identity.status_source = RADEON_STATUS_PCI_RESOURCE_RBBM;
	metadata.identity.status_register = RBBM_STATUS;
	metadata.identity.resource_index = 2;
	metadata.identity.resource_size = 65536;
	snprintf(metadata.identity.drm_driver,
		sizeof(metadata.identity.drm_driver), "radeon");
	metadata.identity.drm_version_major = 2;
	metadata.identity.drm_version_minor = 50;
	metadata.config.ticks = 120;
	metadata.config.dumpinterval = 2;
	metadata.config.dither_seed = 7;
	metadata.vram_size = 134217728;
	metadata.gtt_size = 268435456;
	metadata.sclk_max_khz = 350000;
	metadata.mclk_max_khz = 400000;

	stream = tmpfile();
	CHECK(stream != NULL);
	if (!stream)
		return;

	CHECK(radeontop_capture_write_header(stream, &metadata) == 0);
	CHECK(read_stream(stream, output, sizeof(output)));
	CHECK(strstr(output, "# radeontop_capture_v1 {") == output);
	CHECK(strstr(output, "\\\"line\\nslash\\\\end") != NULL);
	CHECK(strstr(output, "ctl\\u0001x") != NULL);
	CHECK(strstr(output, "byte\\u007f\\u0080\\u00ff") != NULL);
	CHECK(strstr(output, "\"build\":{\"version\":\"v1.4-test\"") != NULL);
	CHECK(strstr(output,
		"\"source_commit\":\"0123456789abcdef0123456789abcdef01234567\"") != NULL);
	CHECK(strstr(output, "\"source_state\":\"clean\"") != NULL);
	CHECK(strstr(output,
		"\"source_manifest_encoding\":\"byte-u00xx\",\"source_manifest\":\"source-fixture\\n\"") != NULL);
	CHECK(strstr(output,
		"\"manifest_encoding\":\"byte-u00xx\",\"manifest\":\"build-fixture\\n\"") != NULL);
	CHECK(strstr(output, "\"bdf\":\"0000:01:05.0\"") != NULL);
	CHECK(strstr(output, "\"device_id\":\"5974\"") != NULL);
	CHECK(strstr(output,
		"\"status_register\":{\"name\":\"RBBM_STATUS\",\"offset\":\"0x00000e40\"}") != NULL);
	CHECK(strstr(output, "\"ticks_per_second\":120") != NULL);
	CHECK(strstr(output, "\"dither_seed\":7") != NULL);
	for (const char *cursor = output; *cursor; cursor++)
		if (*cursor == '\n')
			newlines++;
	CHECK(newlines == 1);

	fclose(stream);
}

static void check_snapshot_evidence(void) {
	struct collector_snapshot snapshot;
	struct engine_masks masks;
	FILE *stream;
	char output[8192];

	memset(&snapshot, 0, sizeof(snapshot));
	memset(&masks, 0, sizeof(masks));
	snapshot.generation = 4;
	snapshot.nominal_slots = 10;
	snapshot.attempted_slots = 9;
	snapshot.missed_slots = 1;
	snapshot.status.valid = 8;
	snapshot.status.failed = 1;
	snapshot.sclk.valid = 2;
	snapshot.sclk.failed = 1;
	snapshot.sclk_mean_khz = 425000.0;
	snapshot.lane_busy[COLLECTOR_LANE_GUI] = 3;
	snapshot.uvd.valid = 0;
	snapshot.lane_busy[COLLECTOR_LANE_UVD] = 0;
	masks.lane[COLLECTOR_LANE_GUI] = 1U << 31;
	masks.lane[COLLECTOR_LANE_UVD] = 1U << 19;
	snapshot.capabilities = COLLECTOR_CAP_STATUS | COLLECTOR_CAP_SCLK |
		COLLECTOR_CAP_VRAM;
	snapshot.fatal = true;
	snapshot.fatal_read_result = COLLECTOR_READ_FATAL;

	stream = tmpfile();
	CHECK(stream != NULL);
	if (!stream)
		return;

	CHECK(radeontop_capture_write_snapshot_evidence(stream,
		"11111111-2222-4333-8444-555555555555", &masks, &snapshot) == 0);
	CHECK(read_stream(stream, output, sizeof(output)));
	CHECK(strstr(output, ", evidence_v1 {") == output);
	CHECK(strstr(output,
		"\"gpu\":{\"busy\":3,\"valid\":8,\"nominal\":10,\"conditional\":0.375000000,\"unconditional\":[0.300000000,0.500000000]}") != NULL);
	CHECK(strstr(output,
		"\"uvd\":{\"busy\":0,\"valid\":0,\"nominal\":10,\"conditional\":null,\"unconditional\":[0.000000000,1.000000000]}") != NULL);
	CHECK(strstr(output, "\"status\":{\"supported\":true") != NULL);
	CHECK(strstr(output, "\"uvd\":{\"supported\":false") != NULL);
	CHECK(strstr(output, "\"sclk\":425000.000000000") != NULL);
	CHECK(strstr(output,
		"\"vram\":{\"supported\":true,\"valid\":false,\"bytes\":null}") != NULL);
	CHECK(strstr(output,
		"\"terminal\":{\"fatal\":true,\"read_result\":2}") != NULL);
	CHECK(strchr(output, '\n') == NULL);

	fclose(stream);
}

static void check_exclusive_stream_lock(void) {
	char path[] = "/tmp/radeontop-capture-lock.XXXXXX";
	int descriptor = mkstemp(path);
	FILE *first;
	pid_t child;
	int child_status = 0;

	CHECK(descriptor >= 0);
	if (descriptor < 0)
		return;
	first = fdopen(descriptor, "a+");
	CHECK(first != NULL);
	if (!first) {
		close(descriptor);
		unlink(path);
		return;
	}

	CHECK(radeontop_capture_lock_stream(first) == 0);
	child = fork();
	CHECK(child >= 0);
	if (child == 0) {
		const int correctly_rejected = freopen(path, "a", stdout) &&
			radeontop_capture_lock_stream(stdout) != 0;

		_exit(correctly_rejected ? 0 : 1);
	}
	if (child > 0) {
		CHECK(waitpid(child, &child_status, 0) == child);
		CHECK(WIFEXITED(child_status));
		CHECK(WEXITSTATUS(child_status) == 0);
	}

	CHECK(radeontop_capture_unlock_stream(first) == 0);
	CHECK(fclose(first) == 0);
	first = fopen(path, "a");
	CHECK(first != NULL);
	if (first) {
		CHECK(radeontop_capture_lock_stream(first) == 0);
		CHECK(radeontop_capture_unlock_stream(first) == 0);
		CHECK(fclose(first) == 0);
	}
	CHECK(unlink(path) == 0);
}

static int emit_json_fixture(void) {
	char encoded_argument[] = {
		'u', 't', 'f', '8', ':', (char) 0xc3, (char) 0xa9,
		':', (char) 0xff, '\0'
	};
	char *arguments[] = { "radeontop", "-d", "-", encoded_argument };
	struct radeontop_capture_metadata metadata;
	struct collector_snapshot snapshot;
	struct collector_snapshot fatal_before_first;
	struct engine_masks masks;

	memset(&metadata, 0, sizeof(metadata));
	memcpy(metadata.run_id, "11111111-2222-4333-8444-555555555555", 37);
	memcpy(metadata.boot_id, "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee", 37);
	snprintf(metadata.kernel_release, sizeof(metadata.kernel_release), "test-kernel");
	snprintf(metadata.source_version, sizeof(metadata.source_version), "v1.4-test");
	snprintf(metadata.source_commit, sizeof(metadata.source_commit),
		"0123456789abcdef0123456789abcdef01234567");
	snprintf(metadata.source_state, sizeof(metadata.source_state), "clean");
	snprintf(metadata.source_sha256, sizeof(metadata.source_sha256), "%s",
		source_manifest_fixture_sha256);
	snprintf(metadata.build_manifest_sha256,
		sizeof(metadata.build_manifest_sha256), "%s",
		build_manifest_fixture_sha256);
	metadata.source_manifest = source_manifest_fixture;
	metadata.build_manifest = build_manifest_fixture;
	snprintf(metadata.family_name, sizeof(metadata.family_name), "RS480");
	metadata.argc = 4;
	metadata.argv = arguments;
	metadata.identity.family = RS480;
	metadata.identity.resource_index = -1;

	memset(&snapshot, 0, sizeof(snapshot));
	memset(&masks, 0, sizeof(masks));
	snapshot.generation = 1;
	snapshot.nominal_slots = 4;
	snapshot.attempted_slots = 3;
	snapshot.missed_slots = 1;
	snapshot.status.valid = 2;
	snapshot.status.failed = 1;
	snapshot.sclk.valid = 2;
	snapshot.sclk.failed = 1;
	snapshot.sclk_mean_khz = 425000.0;
	snapshot.capabilities = COLLECTOR_CAP_STATUS | COLLECTOR_CAP_SCLK |
		COLLECTOR_CAP_VRAM;
	snapshot.fatal = true;
	snapshot.fatal_read_result = COLLECTOR_READ_FATAL;
	masks.lane[COLLECTOR_LANE_GUI] = 1U << 31;
	snapshot.lane_busy[COLLECTOR_LANE_GUI] = 1;
	fatal_before_first = snapshot;
	fatal_before_first.generation = 0;

	if (radeontop_capture_write_header(stdout, &metadata) ||
		fputs("sample", stdout) == EOF ||
		radeontop_capture_write_snapshot_evidence(stdout, metadata.run_id,
			&masks, &snapshot) || fputc('\n', stdout) == EOF ||
		radeontop_capture_write_run_end(stdout, metadata.run_id,
			"collector-fatal", 1, 0, 0, &fatal_before_first))
		return 1;

	return fflush(stdout) ? 1 : 0;
}

static void check_system_metadata(void) {
	char *argv[] = { "radeontop", "-d", "-" };
	struct radeontop_capture_metadata metadata;
	struct radeon_device_identity identity;
	const struct radeontop_build_identity build_identity = {
		.version = "test-version",
		.source_commit = "0123456789abcdef0123456789abcdef01234567",
		.source_state = "clean",
		.source_sha256 = source_manifest_fixture_sha256,
		.source_manifest = source_manifest_fixture,
		.manifest_sha256 = build_manifest_fixture_sha256,
		.build_manifest = build_manifest_fixture
	};
	const struct collector_config config = { 120, 1, 0 };

	radeon_device_identity_init(&identity);
	CHECK(radeontop_capture_metadata_init(&metadata, &build_identity, "RS480",
		3, argv, &identity, &config, 0, 0, 0, 0) == 0);
	CHECK(strlen(metadata.run_id) == 36);
	CHECK(strlen(metadata.boot_id) == 36);
	CHECK(metadata.kernel_release[0] != '\0');
	CHECK(metadata.started_realtime.tv_sec > 0);
	CHECK(metadata.argc == 3);
	CHECK(metadata.argv == argv);
	CHECK(!strcmp(metadata.source_version, "test-version"));
	CHECK(!strcmp(metadata.source_commit, build_identity.source_commit));
	CHECK(!strcmp(metadata.source_state, "clean"));
	CHECK(!strcmp(metadata.source_sha256, build_identity.source_sha256));
	CHECK(!strcmp(metadata.build_manifest_sha256,
		build_identity.manifest_sha256));
	CHECK(metadata.source_manifest == build_identity.source_manifest);
	CHECK(metadata.build_manifest == build_identity.build_manifest);
	CHECK(!strcmp(metadata.family_name, "RS480"));

	{
		struct radeontop_build_identity invalid_identity = build_identity;

		invalid_identity.source_state = "asserted";
		CHECK(radeontop_capture_metadata_init(&metadata, &invalid_identity,
			"RS480", 3, argv, &identity, &config, 0, 0, 0, 0) != 0);
		invalid_identity = build_identity;
		invalid_identity.source_state = "dirty";
		CHECK(radeontop_capture_metadata_init(&metadata, &invalid_identity,
			"RS480", 3, argv, &identity, &config, 0, 0, 0, 0) != 0);
		invalid_identity = build_identity;
		invalid_identity.source_commit = "unknown";
		CHECK(radeontop_capture_metadata_init(&metadata, &invalid_identity,
			"RS480", 3, argv, &identity, &config, 0, 0, 0, 0) != 0);
		invalid_identity = build_identity;
		invalid_identity.source_manifest = NULL;
		CHECK(radeontop_capture_metadata_init(&metadata, &invalid_identity,
			"RS480", 3, argv, &identity, &config, 0, 0, 0, 0) != 0);
		invalid_identity = build_identity;
		invalid_identity.source_sha256 = "short";
		CHECK(radeontop_capture_metadata_init(&metadata, &invalid_identity,
			"RS480", 3, argv, &identity, &config, 0, 0, 0, 0) != 0);
	}
}

int main(int argc, char **argv) {
	if (argc == 2 && !strcmp(argv[1], "--emit-json-fixture"))
		return emit_json_fixture();
	if (argc != 1)
		return 2;

	check_header();
	check_snapshot_evidence();
	check_system_metadata();
	check_exclusive_stream_lock();
	CHECK(radeontop_capture_path_is_stdout("-"));
	CHECK(!radeontop_capture_path_is_stdout("-capture.log"));
	CHECK(!radeontop_capture_path_is_stdout(""));
	CHECK(!radeontop_capture_path_is_stdout("capture.log"));
	CHECK(!radeontop_capture_path_is_stdout(NULL));
	CHECK(radeontop_capture_write_header(NULL, NULL) != 0);
	CHECK(radeontop_capture_write_snapshot_evidence(NULL, NULL, NULL, NULL) != 0);
	CHECK(radeontop_capture_write_run_end(NULL, NULL, NULL, 1, 0, 0, NULL) != 0);

	printf("capture: %u checks, %u failed\n", checks, failures);
	return failures ? 1 : 0;
}
