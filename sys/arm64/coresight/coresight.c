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

MALLOC_DEFINE(M_CORESIGHT, "coresight", "ARM Coresight");

static int
coresight_port_find_endpoint(phandle_t node)
{
	char *name;
	int ret;
	phandle_t child;
	phandle_t xref;

	/* Port found, now find endpoint. */
	for (child = OF_child(node); child != 0; child = OF_peer(child)) {
		ret = OF_getprop_alloc(child, "name", sizeof(*name), (void **)&name);
		if (ret == -1)
			continue;
		if (strcasecmp(name, "endpoint") == 0) {
			printf("Endpoint found\n");
			if (OF_getencprop(child, "remote-endpoint", &xref, sizeof(xref)) == -1) {
				printf("failed\n");
				continue;
			}
			if (OF_getproplen(child, "slave-mode") >= 0) {
				printf("endpoint is slave-mode\n");
			}

			printf("remote-endpoint found\n");
		}
		free(name, M_OFWPROP);
		break;
	}

	return (0);
}

static int
coresight_get_ports(phandle_t node,
    struct coresight_platform_data *pdata)
{
	phandle_t child;
	pcell_t port_reg;
	phandle_t xref;
	char *name;
	int ret;
	phandle_t endpoint_child;
	struct endpoint *endp;

	child = ofw_bus_find_child(node, "ports");
	if (child)
		node = child;

	for (child = OF_child(node); child != 0; child = OF_peer(child)) {
		ret = OF_getprop_alloc(child, "name", sizeof(*name), (void **)&name);
		if (ret == -1)
			continue;

		//printf("name %s, ret %d\n", name, ret);

		if (strcasecmp(name, "port") ||
		    strncasecmp(name, "port@", 6)) {

			if (OF_getencprop(child, "reg", (void *)&port_reg,
				sizeof(port_reg)) > 0) {
			}

			/* Port found */
			if (1 == 0)
				coresight_port_find_endpoint(child);

			endpoint_child = ofw_bus_find_child(child, "endpoint");
			if (endpoint_child) {
				printf("endpoint found\n");
				if (OF_getencprop(endpoint_child, "remote-endpoint", &xref,
				    sizeof(xref)) == -1) {
					printf("failed\n");
					continue;
				}
				endp = malloc(sizeof(struct endpoint), M_CORESIGHT,
				    M_WAITOK | M_ZERO);
				endp->node = OF_node_from_xref(xref);
				if (OF_getproplen(endpoint_child, "slave-mode") >= 0) {
					pdata->in_ports++;
					endp->slave = 1;
				} else {
					pdata->out_ports++;
				}

				mtx_lock(&pdata->mtx_lock);
				TAILQ_INSERT_TAIL(&pdata->endpoints, endp, link);
				mtx_unlock(&pdata->mtx_lock);
			}
		}
	}

	return (0);
}

static int
coresight_get_cpu(phandle_t node,
    struct coresight_platform_data *pdata)
{
	phandle_t cpu_node;
	pcell_t xref;
	pcell_t cpu_reg;

	if (OF_getencprop(node, "cpu", &xref, sizeof(xref)) != -1) {
		//printf("cpu xref found\n");
		cpu_node = OF_node_from_xref(xref);
		if (OF_getencprop(cpu_node, "reg", (void *)&cpu_reg,
			sizeof(cpu_reg)) > 0) {
			//printf("cpu reg found: %d\n", cpu_reg);
			pdata->cpu = cpu_reg;

			return (0);
		}
	}

	return (-1);
}

struct coresight_platform_data *
coresight_get_platform_data(device_t dev)
{
	struct coresight_platform_data *pdata;
	phandle_t node;

	node = ofw_bus_get_node(dev);

	pdata = malloc(sizeof(struct coresight_platform_data),
	    M_CORESIGHT, M_WAITOK | M_ZERO);
	mtx_init(&pdata->mtx_lock, "Coresight Platform Data", NULL, MTX_DEF);
	TAILQ_INIT(&pdata->endpoints);

	coresight_get_cpu(node, pdata);
	coresight_get_ports(node, pdata);

	printf("Total ports: in %d out %d\n", pdata->in_ports, pdata->out_ports);

	return (pdata);
}
