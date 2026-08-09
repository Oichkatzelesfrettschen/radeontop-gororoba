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

#include "rs480_observation.h"

#include <stdio.h>

static unsigned int checks;
static unsigned int failures;

#define CHECK(condition) do { \
	checks++; \
	if (!(condition)) { \
		fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
		failures++; \
	} \
} while (0)

static void consume_complete(struct rs480_gart_parser *parser) {
	rs480_gart_parser_consume(parser, "unrelated = 0x1234\n");
	rs480_gart_parser_consume(parser, "GART_FEATURE_ID = 0x11223344\n");
	rs480_gart_parser_consume(parser, "AGP_BASE_2\t=\t0xaabbccdd\n");
	rs480_gart_parser_consume(parser, "GART_BASE = 4096\n");
}

static void check_complete_input(void) {
	struct rs480_gart_parser parser;
	struct rs480_gart_observed_t observed;

	rs480_gart_parser_init(&parser);
	consume_complete(&parser);
	CHECK(rs480_gart_parser_finish(&parser, &observed));
	CHECK(observed.valid == 1);
	CHECK(observed.agp_base_2 == UINT32_C(0xaabbccdd));
	CHECK(observed.gart_feature_id == UINT32_C(0x11223344));
	CHECK(observed.gart_base == 4096);
}

static void check_missing_input(void) {
	struct rs480_gart_parser parser;
	struct rs480_gart_observed_t observed;

	rs480_gart_parser_init(&parser);
	rs480_gart_parser_consume(&parser, "AGP_BASE_2 = 1\n");
	rs480_gart_parser_consume(&parser, "GART_BASE = 2\n");
	CHECK(!rs480_gart_parser_finish(&parser, &observed));
}

static void check_rejected_line(const char *line) {
	struct rs480_gart_parser parser;
	struct rs480_gart_observed_t observed;

	rs480_gart_parser_init(&parser);
	rs480_gart_parser_consume(&parser, line);
	CHECK(parser.malformed);
	CHECK(!rs480_gart_parser_finish(&parser, &observed));
}

static void check_duplicate_input(void) {
	struct rs480_gart_parser parser;
	struct rs480_gart_observed_t observed;

	rs480_gart_parser_init(&parser);
	consume_complete(&parser);
	rs480_gart_parser_consume(&parser, "AGP_BASE_2 = 9\n");
	CHECK(parser.malformed);
	CHECK(!rs480_gart_parser_finish(&parser, &observed));
}

int main(void) {
	check_complete_input();
	check_missing_input();
	check_duplicate_input();
	check_rejected_line("GART_BASE_ALIAS = 9\n");
	check_rejected_line("GART_FEATURE_ID = 9junk\n");
	check_rejected_line("GART_BASE = 0x100000000\n");
	check_rejected_line("GART_BASE = -1\n");
	check_rejected_line("GART_BASE = +1\n");
	check_rejected_line("GART_BASE=1\n");
	check_rejected_line("AGP_BASE_2 garbage = 1\n");
	CHECK(!rs480_gart_parser_finish(NULL, NULL));

	printf("RS480 observation parser: %u checks, %u failed\n",
		checks, failures);
	return failures ? 1 : 0;
}
