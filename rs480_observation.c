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

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

enum {
	RS480_GART_AGP_BASE_2 = 1U << 0,
	RS480_GART_FEATURE_ID = 1U << 1,
	RS480_GART_BASE = 1U << 2,
	RS480_GART_COMPLETE = RS480_GART_AGP_BASE_2 |
		RS480_GART_FEATURE_ID | RS480_GART_BASE
};

void rs480_gart_parser_init(struct rs480_gart_parser *parser) {
	memset(parser, 0, sizeof(*parser));
}

static bool parse_value(const char *line, const char *key, uint32_t *out) {
	const size_t key_length = strlen(key);
	const char *cursor;
	char *end = NULL;
	unsigned long value;

	if (strncmp(line, key, key_length) ||
		(line[key_length] != ' ' && line[key_length] != '\t'))
		return false;

	cursor = line + key_length;
	while (*cursor == ' ' || *cursor == '\t')
		cursor++;
	if (*cursor++ != '=')
		return false;
	while (*cursor == ' ' || *cursor == '\t')
		cursor++;
	if (*cursor == '-' || *cursor == '+')
		return false;

	errno = 0;
	value = strtoul(cursor, &end, 0);
	if (errno || end == cursor)
		return false;
#if ULONG_MAX > UINT32_MAX
	if (value > UINT32_MAX)
		return false;
#endif

	while (*end && isspace((unsigned char) *end))
		end++;
	if (*end)
		return false;

	*out = (uint32_t) value;
	return true;
}

static bool consume_key(struct rs480_gart_parser *parser, const char *line,
		const char *key, unsigned int field, uint32_t *value) {
	const size_t key_length = strlen(key);

	if (strncmp(line, key, key_length))
		return false;

	if ((parser->found & field) || !parse_value(line, key, value))
		parser->malformed = true;
	parser->found |= field;
	return true;
}

void rs480_gart_parser_consume(struct rs480_gart_parser *parser,
		const char *line) {
	if (!parser || !line)
		return;

	if (consume_key(parser, line, "AGP_BASE_2", RS480_GART_AGP_BASE_2,
			&parser->observed.agp_base_2))
		return;
	if (consume_key(parser, line, "GART_FEATURE_ID", RS480_GART_FEATURE_ID,
			&parser->observed.gart_feature_id))
		return;
	consume_key(parser, line, "GART_BASE", RS480_GART_BASE,
		&parser->observed.gart_base);
}

bool rs480_gart_parser_finish(const struct rs480_gart_parser *parser,
		struct rs480_gart_observed_t *observed) {
	if (!parser || !observed || parser->malformed ||
		parser->found != RS480_GART_COMPLETE)
		return false;

	*observed = parser->observed;
	observed->valid = 1;
	return true;
}
