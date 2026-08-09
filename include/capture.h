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

#ifndef CAPTURE_H
#define CAPTURE_H

#include "collector.h"
#include "device_model.h"

#include <stdio.h>

#define RADEONTOP_CAPTURE_UUID_SIZE 37
#define RADEONTOP_CAPTURE_SHA256_SIZE 65
#define RADEONTOP_CAPTURE_GIT_OID_SIZE 65

struct radeontop_build_identity {
	const char *version;
	const char *source_commit;
	const char *source_state;
	const char *source_sha256;
	const char *source_manifest;
	const char *manifest_sha256;
	const char *build_manifest;
};

struct radeontop_capture_metadata {
	char run_id[RADEONTOP_CAPTURE_UUID_SIZE];
	char boot_id[RADEONTOP_CAPTURE_UUID_SIZE];
	char kernel_release[256];
	char source_version[128];
	char source_commit[RADEONTOP_CAPTURE_GIT_OID_SIZE];
	char source_state[8];
	char source_sha256[RADEONTOP_CAPTURE_SHA256_SIZE];
	char build_manifest_sha256[RADEONTOP_CAPTURE_SHA256_SIZE];
	const char *source_manifest;
	const char *build_manifest;
	char family_name[32];
	struct timespec started_realtime;
	struct timespec started_monotonic;
	int argc;
	// argv borrows the caller's vector and byte strings.  They remain alive and
	// unchanged through every capture-header write that uses this metadata.
	char *const *argv;
	struct radeon_device_identity identity;
	struct collector_config config;
	uint64_t vram_size;
	uint64_t gtt_size;
	uint32_t sclk_max_khz;
	uint32_t mclk_max_khz;
};

int radeontop_capture_metadata_init(struct radeontop_capture_metadata *metadata,
		const struct radeontop_build_identity *build_identity,
		const char *family_name,
		int argc, char *const argv[],
		const struct radeon_device_identity *identity,
		const struct collector_config *config,
		uint64_t vram_size, uint64_t gtt_size,
		uint32_t sclk_max_khz, uint32_t mclk_max_khz);

int radeontop_capture_write_header(FILE *stream,
		const struct radeontop_capture_metadata *metadata);

bool radeontop_capture_path_is_stdout(const char *path);
int radeontop_capture_lock_stream(FILE *stream);
int radeontop_capture_unlock_stream(FILE *stream);
int radeontop_capture_sync_stream(FILE *stream);

// A nonempty regular stream receives one newline before the next run header.
// The boundary keeps a truncated prior record from absorbing that header.
int radeontop_capture_write_append_boundary(FILE *stream);

// Appends a machine-readable object to the legacy dump line.  The caller owns
// the final newline so both representations describe one atomic window record.
int radeontop_capture_write_snapshot_evidence(FILE *stream,
		const char *run_id,
		const struct engine_masks *masks,
		const struct collector_snapshot *snapshot);

int radeontop_capture_write_run_end(FILE *stream, const char *run_id,
		const char *reason, int exit_status, uint64_t last_generation,
		unsigned int record_count,
		const struct collector_snapshot *collector_snapshot,
		const struct collector_terminal *terminal);

#endif
