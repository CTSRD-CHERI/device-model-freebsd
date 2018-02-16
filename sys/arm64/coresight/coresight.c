/*-
 * Copyright (c) 2018 Ruslan Bukin <br@bsdpad.com>
 * All rights reserved.
 *
 * This software was developed by BAE Systems, the University of Cambridge
 * Computer Laboratory, and Memorial University under DARPA/AFRL contract
 * FA8650-15-C-7558 ("CADETS"), as part of the DARPA Transparent Computing
 * (TC) research program.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/rman.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <machine/bus.h>

#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

#include <arm64/coresight/coresight.h>

int
coresight_parse_port(phandle_t node)
{
	char *name;
	int ret;
	phandle_t child;
	phandle_t xref;

	for (child = OF_child(node); child != 0; child = OF_peer(child)) {
		ret = OF_getprop_alloc(child, "name", sizeof(*name), (void **)&name);
		if (ret == -1)
			continue;
		if (strcmp(name, "endpoint") == 0) {
			printf("Endpoint found\n");
			if (OF_getencprop(child, "remote-endpoint", &xref, sizeof(xref)) == -1) {
				printf("failed\n");
				continue;
			}
			printf("remote-endpoint found\n");
		}
	}

	return (0);
}

int
coresight_parse_ports(device_t dev)
{
	phandle_t node;
	phandle_t child;
	char *name;
	int ret;

	node = ofw_bus_get_node(dev);

	child = ofw_bus_find_child(node, "ports");
	if (child)
		node = child;

	for (child = OF_child(node); child != 0; child = OF_peer(child)) {
		ret = OF_getprop_alloc(child, "name", sizeof(*name), (void **)&name);
		if (ret == -1)
			continue;
		if (strcmp(name, "port") == 0) {
			printf("Port found\n");
			coresight_parse_port(child);
		}
	}

	return (0);
}
