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

#ifndef RS480_OBSERVATION_H
#define RS480_OBSERVATION_H

#include <stdbool.h>
#include <stdint.h>

struct rs480_gart_observed_t {
	unsigned char valid;
	uint32_t agp_base_2;
	uint32_t gart_feature_id;
	uint32_t gart_base;
};

struct rs480_gart_parser {
	struct rs480_gart_observed_t observed;
	unsigned int found;
	bool malformed;
};

void rs480_gart_parser_init(struct rs480_gart_parser *parser);
void rs480_gart_parser_consume(struct rs480_gart_parser *parser,
		const char *line);
bool rs480_gart_parser_finish(const struct rs480_gart_parser *parser,
		struct rs480_gart_observed_t *observed);

#endif
