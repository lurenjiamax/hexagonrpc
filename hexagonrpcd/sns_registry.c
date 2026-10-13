/*
 * Qualcomm sensor registry property interface
 *
 * Copyright (C) 2026 The HexagonRPC Contributors
 *
 * This file is part of HexagonRPC.
 *
 * HexagonRPC is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "interfaces/sns_registry.def"
#include "listener.h"
#include "sns_registry.h"

#define PROPERTY_VALUE_MAX 92

static uint32_t sns_registry_get_property(void *data,
					  const struct fastrpc_io_buffer *inbufs,
					  struct fastrpc_io_buffer *outbufs)
{
	const char *name = inbufs[1].p;
	char *value = outbufs[0].p;

	if (name == NULL || inbufs[1].s == 0 ||
	    name[inbufs[1].s - 1] != '\0' ||
	    value == NULL || outbufs[0].s < PROPERTY_VALUE_MAX)
		return (uint32_t) -1;

	/*
	 * Android's property_get() returns its default value when a property
	 * does not exist. SensorsPD supplies an empty default and only uses
	 * these properties as optional registry-generation selectors.
	 */
	memset(value, 0, outbufs[0].s);

#if HEXAGONRPC_VERBOSE
	fprintf(stderr, "sns_registry_get_property(%s) -> \"\"\n", name);
#endif

	return 0;
}

struct fastrpc_interface *fastrpc_sns_registry_init(void)
{
	struct fastrpc_interface *iface = malloc(sizeof(*iface));

	if (iface != NULL)
		memcpy(iface, &sns_registry_interface, sizeof(*iface));

	return iface;
}

void fastrpc_sns_registry_deinit(struct fastrpc_interface *iface)
{
	free(iface);
}

static const struct fastrpc_function_impl sns_registry_procs[] = {
	{ .def = &sns_registry_get_property_def,
	  .impl = sns_registry_get_property, },
};

const struct fastrpc_interface sns_registry_interface = {
	.name = "sns_registry",
	.n_procs = 1,
	.procs = sns_registry_procs,
};
