/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2019 Ruslan Bukin <br@bsdpad.com>
 *
 * This software was developed by SRI International and the University of
 * Cambridge Computer Laboratory (Department of Computer Science and
 * Technology) under DARPA contract HR0011-18-C-0016 ("ECATS"), as part of the
 * DARPA SSITH research programme.
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

/*
 * Intel Stratix 10 SVC
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/malloc.h>
#include <sys/rman.h>
#include <sys/timeet.h>
#include <sys/timetc.h>
#include <sys/conf.h>
#include <sys/uio.h>
#include <sys/vmem.h>

#include <dev/fdt/simplebus.h>
#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

#include <arm64/intel/intel-smc.h>
#include <arm64/intel/stratix10-svc.h>

#include <machine/bus.h>
#include <machine/cpu.h>
#include <machine/intr.h>

struct s10_svc_softc {
	device_t		dev;
	vmem_t			*vmem;
};

int
s10_svc_send(void)
{
	uint64_t ret;
	register_t a0, a1, a2;
	struct arm_smccc_res res;

	printf("%s\n", __func__);

	a0 = INTEL_SIP_SMC_FPGA_CONFIG_START;
	a1 = 1; //flag partial ?

	a0 = INTEL_SIP_SMC_RSU_STATUS;
	a0 = INTEL_SIP_SMC_FPGA_CONFIG_GET_MEM;
	a1 = 0;
	a2 = 0;

	ret = arm_smccc_smc(a0, a1, a2, 0, 0, 0, 0, 0, &res);

	printf("res.a0 %lx, a1 %lx, a2 %lx, a3 %lx\n",
	    res.a0, res.a1, res.a2, res.a3);

	return (0);
}

static int
s10_get_memory(struct s10_svc_softc *sc)
{
	struct arm_smccc_res res;
	vmem_addr_t addr;
	vmem_size_t size;
	vmem_t *vmem;

	arm_smccc_smc(INTEL_SIP_SMC_FPGA_CONFIG_GET_MEM,
	    0, 0, 0, 0, 0, 0, 0, &res);

	if (res.a0 != 0)
		return (ENXIO);

	vmem = vmem_create("stratix10 vmem", 0, 0, PAGE_SIZE,
	    PAGE_SIZE, M_BESTFIT | M_WAITOK);
	if (vmem == NULL)
		return (ENXIO);

	addr = res.a1;
	size = res.a2;

	printf("%s: shared memory addr %lx size %lx\n", __func__,
	    addr, size);
	vmem_add(vmem, addr, size, 0);

	sc->vmem = vmem;

	return (0);
}

static int
s10_svc_probe(device_t dev)
{

	if (!ofw_bus_status_okay(dev))
		return (ENXIO);

	if (!ofw_bus_is_compatible(dev, "intel,stratix10-svc"))
		return (ENXIO);

	device_set_desc(dev, "Stratix 10 SVC");

	return (BUS_PROBE_DEFAULT);
}

static int
s10_svc_attach(device_t dev)
{
	struct s10_svc_softc *sc;

	sc = device_get_softc(dev);
	sc->dev = dev;

	if (s10_get_memory(sc) != 0)
		return (ENXIO);

	return (0);
}

static device_method_t s10_svc_methods[] = {
	DEVMETHOD(device_probe,		s10_svc_probe),
	DEVMETHOD(device_attach,	s10_svc_attach),
	{ 0, 0 }
};

static driver_t s10_svc_driver = {
	"s10_svc",
	s10_svc_methods,
	sizeof(struct s10_svc_softc),
};

static devclass_t s10_svc_devclass;

DRIVER_MODULE(s10_svc, firmware, s10_svc_driver,
    s10_svc_devclass, 0, 0);
