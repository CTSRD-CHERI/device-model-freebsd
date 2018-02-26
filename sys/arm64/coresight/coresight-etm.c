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

#include <arm64/coresight/coresight.h>

extern struct coresight_device_list cs_devs;

#define	CORESIGHT_DISABLE	0
#define	CORESIGHT_ENABLE	1
#define	CORESIGHT_READ		2

static int
coresight_build_path_one(struct coresight_device *out,
    struct endpoint *out_endp, struct coresight_event *event, uint8_t cmd)
{

	//printf("%s\n", __func__);

	switch (out->dev_type) {
	//case CORESIGHT_ETMV4:
	//	out->ops->source_ops->enable(config);
	//	break;
	case CORESIGHT_ETF:
		break;
	case CORESIGHT_ETR:
		//printf("enabling SINK ops\n");
		switch (cmd) {
		case CORESIGHT_DISABLE:
			out->ops->sink_ops->disable(out, event);
			break;
		case CORESIGHT_ENABLE:
			out->ops->sink_ops->enable(out, out_endp, event);
			break;
		case CORESIGHT_READ:
			out->ops->sink_ops->read(out, out_endp, event);
			break;
		};
		break;
	case CORESIGHT_DYNAMIC_REPLICATOR:
	case CORESIGHT_FUNNEL:
		//printf("enabling LINK ops\n");
		switch (cmd) {
		case CORESIGHT_DISABLE:
			out->ops->link_ops->disable(out, out_endp);
			break;
		case CORESIGHT_ENABLE:
			out->ops->link_ops->enable(out, out_endp);
			break;
		};
		break;
	default:
		break;
	}

	//printf("%s: done\n", __func__);

	return (0);
}

static struct coresight_device *
coresight_build_path0(struct coresight_device *out,
    struct coresight_event *event, uint8_t cmd)
{
	struct endpoint *out_endp;
	struct endpoint *endp;

	TAILQ_FOREACH(endp, &out->pdata->endpoints, link) {
		if (endp->slave != 0)
			continue;

		out = coresight_get_output_device(endp, &out_endp);
		if (out) {
			coresight_build_path_one(out, out_endp, event, cmd);

			/* Sink device found, stop iteration */
			if (out->dev_type == event->sink)
				return (NULL);
		}
	}

	return (out);
}

static int
coresight_build_path(struct coresight_device *cs_dev,
    struct coresight_event *event, uint8_t cmd)
{
	struct coresight_device *out;

	out = cs_dev;
	while (out)
		out = coresight_build_path0(out, event, cmd);

	return (0);
}

int
coresight_disable(int cpu, struct coresight_event *event)
{
	struct coresight_device *cs_dev;

	TAILQ_FOREACH(cs_dev, &cs_devs, link) {
		if (cs_dev->dev_type == event->src &&
		    cs_dev->pdata->cpu == cpu) {

			cs_dev->ops->source_ops->disable(cs_dev);
			coresight_build_path(cs_dev, event, CORESIGHT_DISABLE);
			break;
		}
	}

	return (0);
}

int
coresight_enable(int cpu, struct coresight_event *event)
{
	struct coresight_device *cs_dev;

	TAILQ_FOREACH(cs_dev, &cs_devs, link) {
		if (cs_dev->dev_type == event->src &&
		    cs_dev->pdata->cpu == cpu) {

			coresight_build_path(cs_dev, event, CORESIGHT_ENABLE);
			cs_dev->ops->source_ops->enable(cs_dev, event);
			break;
		}
	}

	return (0);
}

int
coresight_read(int cpu, struct coresight_event *event)
{
	struct coresight_device *cs_dev;

	TAILQ_FOREACH(cs_dev, &cs_devs, link) {
		if (cs_dev->dev_type == event->src &&
		    cs_dev->pdata->cpu == cpu) {

			coresight_build_path(cs_dev, event, CORESIGHT_READ);
			//cs_dev->ops->source_ops->read(cs_dev, event);
			break;
		}
	}

	return (0);
}
