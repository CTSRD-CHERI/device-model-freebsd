/*-
 * Copyright (c) 2017 Ruslan Bukin <br@bsdpad.com>
 * All rights reserved.
 *
 * This software was developed by SRI International and the University of
 * Cambridge Computer Laboratory under DARPA/AFRL contract FA8750-10-C-0237
 * ("CTSRD"), as part of the DARPA CRASH research programme.
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

/* ARM PrimeCell DMA Controller (PL330) driver. */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include "opt_platform.h"
#include <sys/param.h>
#include <sys/endian.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/kthread.h>
#include <sys/sglist.h>
#include <sys/module.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/resource.h>
#include <sys/rman.h>

#include <vm/vm.h>
#include <vm/vm_extern.h>
#include <vm/vm_kern.h>
#include <vm/pmap.h>

#include <machine/bus.h>
#include <machine/fdt.h>
//#include <machine/cache.h>

#ifdef FDT
#include <dev/fdt/fdt_common.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>
#endif

#include <dev/xdma/xdma.h>
#include "xdma_if.h"

#include <dev/xdma/controller/pl330.h>

#define	MSGDMA_NCHANNELS	32

struct pl330_channel {
	struct pl330_softc	*sc;
	struct mtx		mtx;
	xdma_channel_t		*xchan;
	struct proc		*p;
	int			used;
	int			index;
	int			idx_head;
	int			idx_tail;

	uint8_t			*ibuf;
	bus_addr_t		ibuf_phys;

	uint32_t		enqueued;
	uint32_t		capacity;
};

struct pl330_fdt_data {
	uint32_t periph_id;
};

struct pl330_softc {
	device_t		dev;
	struct resource		*res[10];
	bus_space_tag_t		bst;
	bus_space_handle_t	bsh;
	void			*ih;
	struct pl330_channel	channels[MSGDMA_NCHANNELS];
};

static struct resource_spec pl330_spec[] = {
	{ SYS_RES_MEMORY,	0,	RF_ACTIVE },
	{ SYS_RES_IRQ,		0,	RF_ACTIVE },
	{ SYS_RES_IRQ,		1,	RF_ACTIVE },
	{ SYS_RES_IRQ,		2,	RF_ACTIVE },
	{ SYS_RES_IRQ,		3,	RF_ACTIVE },
	{ SYS_RES_IRQ,		4,	RF_ACTIVE },
	{ SYS_RES_IRQ,		5,	RF_ACTIVE },
	{ SYS_RES_IRQ,		6,	RF_ACTIVE },
	{ SYS_RES_IRQ,		7,	RF_ACTIVE },
	{ SYS_RES_IRQ,		8,	RF_ACTIVE },
	{ -1, 0 }
};

#define	HWTYPE_NONE	0
#define	HWTYPE_STD	1

static struct ofw_compat_data compat_data[] = {
	{ "arm,pl330",		HWTYPE_STD },
	{ NULL,			HWTYPE_NONE },
};

static int pl330_probe(device_t dev);
static int pl330_attach(device_t dev);
static int pl330_detach(device_t dev);

static void
pl330_intr(void *arg)
{
	xdma_transfer_status_t status;
	struct xdma_transfer_status st;
	struct pl330_channel *chan;
	struct xdma_channel *xchan;
	struct pl330_softc *sc;
	uint32_t pending;
	int i;
	int c;

	sc = arg;

	pending = READ4(sc, INTMIS);
#if 0
	printf("%s: 0x%x, LC0 %x, SAR %x DAR %x\n",
	    __func__, pending, READ4(sc, LC0(0)),
	    READ4(sc, SAR(0)), READ4(sc, DAR(0)));
#endif
	WRITE4(sc, INTCLR, pending);

	for (c = 0; c < 32; c++) {
		if ((pending & (1 << c)) == 0) {
			continue;
		}
		chan = &sc->channels[c];
		xchan = chan->xchan;
		st.error = 0;
		st.transferred = 0;
		for (i = 0; i < chan->enqueued; i++) {
			//printf("seg done\n");
			xchan_seg_done(xchan, &st);
		}

		chan->capacity = 0x10000;

		/* Finish operation */
		status.error = 0;
		status.transferred = 0; //tot_copied;
		xdma_callback(chan->xchan, &status);
	}
}

static uint32_t
emit_mov(uint8_t *buf, uint32_t reg, uint32_t val)
{

	buf[0] = DMAMOV;

	buf[1] = reg;
	buf[2] = val;
	buf[3] = val >> 8;
	buf[4] = val >> 16;
	buf[5] = val >> 24;

	return (6);
}

static uint32_t
emit_lp(uint8_t *buf, uint8_t idx, uint32_t iter)
{

	if (idx > 1) {
		return (0);
	}

	buf[0] = DMALP;
	buf[0] |= (idx << 1);

	buf[1] = (iter - 1) & 0xff;

	return (2);
}

static uint32_t
emit_lpend(uint8_t *buf, uint8_t idx, uint8_t jump_addr_relative)
{

	buf[0] = DMALPEND;
	buf[0] |= (1 << 4); //dmalp started the loop
	buf[0] |= (idx << 2);

	//buf[0] |= (0 << 1) | (1 << 0); //single
	buf[0] |= (1 << 1) | (1 << 0); //burst
	buf[1] = jump_addr_relative;

	return (2);
}

static uint32_t
emit_ld(uint8_t *buf)
{

	buf[0] = DMALD;
	//single
	//buf[0] |= (0 << 1) | (1 << 0);

	//burst
	buf[0] |= (1 << 1) | (1 << 0);

	return (1);
}

static uint32_t
emit_st(uint8_t *buf)
{

	buf[0] = DMAST;
	//single
	//buf[0] |= (0 << 1) | (1 << 0);
	//burst
	buf[0] |= (1 << 1) | (1 << 0);

	return (1);
}

static uint32_t
emit_end(uint8_t *buf)
{

	buf[0] = DMAEND;

	return (1);
}

static uint32_t
emit_sev(uint8_t *buf, uint32_t ev)
{

	buf[0] = DMASEV;
	buf[1] = (ev << 3);

	return (2);
}

static uint32_t
emit_wfp(uint8_t *buf, uint32_t p_id)
{

	buf[0] = DMAWFP;
	buf[0] |= (1 << 0); //periph
	buf[1] = (p_id << 3);

	return (2);
}

static uint32_t
emit_go(uint8_t *buf, uint32_t chan_id, uint32_t addr)
{

	buf[0] = DMAGO;
	//buf[0] |= (1 << 1); //ns

	buf[1] = chan_id;
	buf[2] = addr;
	buf[3] = addr >> 8;
	buf[4] = addr >> 16;
	buf[5] = addr >> 24;

	return (6);
}

static int
pl330_test(struct pl330_softc *sc)
{
	uint8_t *ibuf;
	uint8_t dbuf[6];
	uint8_t *buf1;
	uint8_t *buf2;
	uint32_t offs;
	uint32_t reg;

	bus_space_handle_t sram;
	if (bus_space_map(fdtbus_bs_tag, 0xFFE00000, 1024, 0, &sram) != 0) {
		printf("failed\n");
	}
	bus_space_write_4(fdtbus_bs_tag, sram, 0, 0xab);

	WRITE4(sc, INTEN, (1 << 0));

	printf("CRD %x\n", READ4(sc, CRD));

#if 0
	dbuf = (void *)kmem_alloc_contig(kernel_arena,
		PAGE_SIZE, M_ZERO, 0, ~0, PAGE_SIZE, 0,
		VM_MEMATTR_UNCACHEABLE);
#endif

	ibuf = (void *)kmem_alloc_contig(kernel_arena,
		PAGE_SIZE, M_ZERO, 0, ~0, PAGE_SIZE, 0,
		VM_MEMATTR_UNCACHEABLE);
	printf("ibuf is %x\n", (uint32_t)ibuf);

	buf1 = (void *)kmem_alloc_contig(kernel_arena,
		PAGE_SIZE, M_ZERO, 0, ~0, PAGE_SIZE, 0,
		VM_MEMATTR_UNCACHEABLE);
	buf1[0] = 0xaa;

	buf2 = (void *)kmem_alloc_contig(kernel_arena,
		PAGE_SIZE, M_ZERO, 0, ~0, PAGE_SIZE, 0,
		VM_MEMATTR_UNCACHEABLE);

	printf("buf1 %x\n", vtophys(buf1));
	printf("buf2 %x\n", vtophys(buf2));
	printf("ibuf %x\n", vtophys(ibuf));

	reg = (1 << 8) | (1 << 9) | (1 << 10);
	reg |= (1 << 22) | (1 << 23) | (1 << 24);

	reg = (1 << 0) | (1 << 14);

	//SS32, DS32
	reg |= (2 << 1); //0b010 = reads 4 bytes per beat
	reg |= (2 << 15); //0b010 = writes 4 bytes per beat

	//SS64, DS64
	//reg |= (3 << 1); //0b011 = reads 8 bytes per beat
	//reg |= (3 << 15); //0b011 = writes 8 bytes per beat

	offs = 0;
	offs += emit_mov(&ibuf[offs], R_CCR, reg);

	//offs += emit_mov(&ibuf[offs], R_SAR, 0xFFE00000); //sram
	//offs += emit_mov(&ibuf[offs], R_DAR, 0xFFE00010); //sram
	offs += emit_mov(&ibuf[offs], R_SAR, 0xffa00000); //vtophys(buf1));
	offs += emit_mov(&ibuf[offs], R_DAR, vtophys(buf2));

	offs += emit_wfp(&ibuf[offs], 25); //25 -- qspi rx
	offs += emit_ld(&ibuf[offs]);
	offs += emit_st(&ibuf[offs]);
	offs += emit_sev(&ibuf[offs], 0);
	offs += emit_end(&ibuf[offs]);

	emit_go(dbuf, 0, vtophys(ibuf));

	reg = (dbuf[1] << 24) | (dbuf[0] << 16);
	WRITE4(sc, DBGINST0, reg);
	reg = (dbuf[5] << 24) | (dbuf[4] << 16) | (dbuf[3] << 8) | dbuf[2];
	WRITE4(sc, DBGINST1, reg);

	printf("DSR %x, DBGSTATUS %x FTRD %x FTR(0) %x\n",
	    READ4(sc, DSR), READ4(sc, DBGSTATUS), READ4(sc, FTRD), READ4(sc, FTR(0)));
	WRITE4(sc, DBGCMD, 0);
	DELAY(100000);
	DELAY(100000);
	DELAY(100000);
	DELAY(100000);
	DELAY(100000);
	DELAY(100000);
	printf("DSR %x, DBGSTATUS %x FTRD %x FTR(0) %x\n",
	    READ4(sc, DSR), READ4(sc, DBGSTATUS), READ4(sc, FTRD), READ4(sc, FTR(0)));
	printf("CSR(0) %x\n", READ4(sc, CSR(0)));
	printf("RESULT: buf2 is %x\n", buf2[0]);

	printf("%s: SAR(0) %x\n", __func__, READ4(sc, SAR(0)));
	printf("%s: DAR(0) %x\n", __func__, READ4(sc, DAR(0)));
	printf("%s: CCR(0) %x\n", __func__, READ4(sc, CCR(0)));

	printf("res %x\n", bus_space_read_4(fdtbus_bs_tag, sram, 0x10));

	return (0);
}

static int
pl330_probe(device_t dev)
{
	int hwtype;

	if (!ofw_bus_status_okay(dev))
		return (ENXIO);

	hwtype = ofw_bus_search_compatible(dev, compat_data)->ocd_data;
	if (hwtype == HWTYPE_NONE)
		return (ENXIO);

	device_set_desc(dev, "ARM PrimeCell DMA Controller (PL330)");

	return (BUS_PROBE_DEFAULT);
}

static int
pl330_attach(device_t dev)
{
	struct pl330_softc *sc;
	phandle_t xref, node;
	int err;

	sc = device_get_softc(dev);
	sc->dev = dev;

	if (bus_alloc_resources(dev, pl330_spec, sc->res)) {
		device_printf(dev, "could not allocate resources for device\n");
		return (ENXIO);
	}

	/* CSR memory interface */
	sc->bst = rman_get_bustag(sc->res[0]);
	sc->bsh = rman_get_bushandle(sc->res[0]);

	/* Setup interrupt handler */
	err = bus_setup_intr(dev, sc->res[1], INTR_TYPE_MISC | INTR_MPSAFE,
	    NULL, pl330_intr, sc, &sc->ih);
	if (err) {
		device_printf(dev, "Unable to alloc interrupt resource.\n");
		return (ENXIO);
	}

	/* Setup interrupt handler */
	err = bus_setup_intr(dev, sc->res[2], INTR_TYPE_MISC | INTR_MPSAFE,
	    NULL, pl330_intr, sc, &sc->ih);
	if (err) {
		device_printf(dev, "Unable to alloc interrupt resource.\n");
		return (ENXIO);
	}

	node = ofw_bus_get_node(dev);
	xref = OF_xref_from_node(node);
	OF_device_register_xref(xref, dev);

	uint32_t reg;

	reg = READ4(sc, CRD);
	reg &= ~(0x7);
	reg |= 0x2;
	WRITE4(sc, CRD, reg);

	if (0 == 1) {
		pl330_test(sc);
	}

#if 0
	printf("%s: read status: %x\n", __func__, READ4(sc, 0x00));
	printf("%s: read control: %x\n", __func__, READ4(sc, 0x04));
	printf("%s: read 1: %x\n", __func__, READ4(sc, 0x08));
	printf("%s: read 2: %x\n", __func__, READ4(sc, 0x0C));

	int timeout;

	WRITE4(sc, DMA_STATUS, 0x3ff);
	WRITE4(sc, DMA_CONTROL, CONTROL_RESET);

	timeout = 100;
	do {
		if ((READ4(sc, DMA_STATUS) & STATUS_RESETTING) == 0)
			break;
	} while (timeout--);

	printf("timeout %d\n", timeout);

	WRITE4(sc, DMA_CONTROL, CONTROL_GIEM);

	printf("%s: read control after reset: %x\n", __func__, READ4(sc, DMA_CONTROL));

	int i;
	for (i = 0; i < 10000; i++) {
		printf("%s: read control after reset: %x\n", __func__, READ4(sc, DMA_CONTROL));
		DELAY(1);
	}

	for (i = 0; i < 20; i++) {
		printf("%s: read status after reset: %x\n", __func__, READ4(sc, DMA_STATUS));
	}
#endif

	return (0);
}

static int
pl330_detach(device_t dev)
{
	struct pl330_softc *sc;

	sc = device_get_softc(dev);

	return (0);
}

static int
pl330_channel_alloc(device_t dev, struct xdma_channel *xchan)
{
	struct pl330_channel *chan;
	struct pl330_softc *sc;
	int i;

	sc = device_get_softc(dev);

	for (i = 0; i < MSGDMA_NCHANNELS; i++) {
		chan = &sc->channels[i];
		if (chan->used == 0) {
			chan->xchan = xchan;
			xchan->chan = (void *)chan;
			xchan->caps |= XCHAN_CAP_BUSDMA;
			chan->index = i;
			chan->sc = sc;
			chan->used = 1;
			chan->idx_head = 0;
			chan->idx_tail = 0;

			chan->ibuf = (void *)kmem_alloc_contig(kernel_arena,
			    PAGE_SIZE, M_ZERO, 0, ~0, PAGE_SIZE, 0,
			    VM_MEMATTR_UNCACHEABLE);
			chan->ibuf_phys = vtophys(chan->ibuf);

			return (0);
		}
	}

	return (-1);
}

static int
pl330_channel_free(device_t dev, struct xdma_channel *xchan)
{
	struct pl330_channel *chan;
	struct pl330_softc *sc;

	sc = device_get_softc(dev);

	chan = (struct pl330_channel *)xchan->chan;
	chan->used = 0;

	return (0);
}

static int
pl330_channel_capacity(device_t dev, xdma_channel_t *xchan,
    uint32_t *capacity)
{
	struct pl330_channel *chan;
	//uint32_t c;

	chan = (struct pl330_channel *)xchan->chan;

	*capacity = chan->capacity;

	return (0);
}

static int
pl330_channel_submit_sg(device_t dev, struct xdma_channel *xchan,
    struct xdma_sglist *sg, uint32_t sg_n)
{
	xdma_controller_t *xdma;
	struct pl330_channel *chan;
	struct pl330_softc *sc;
	uint32_t src_addr_lo;
	uint32_t dst_addr_lo;
	uint8_t *ibuf;
	uint32_t len;
	uint32_t reg;
	uint32_t offs;
	uint8_t dbuf[6];
	uint8_t jump_addr_relative;
	uint8_t offs0, offs1;
	uint32_t cnt;
	struct pl330_fdt_data *data;
	int i;

	sc = device_get_softc(dev);

	xdma = xchan->xdma;
	data = (struct pl330_fdt_data *)xdma->data;

	chan = (struct pl330_channel *)xchan->chan;
	ibuf = chan->ibuf;

	//printf("%s: chan->index %d\n", __func__, chan->index);

	offs = 0;

	for (i = 0; i < sg_n; i++) {
		if (sg[i].direction == XDMA_DEV_TO_MEM) {
			reg = (1 << 14); //dst inc
			//printf("dst inc\n");
		} else {
			//printf("src inc\n");
			reg = (1 << 0); //src inc
			reg |= (1 << 22); //dst prot ctrl

		}

		//SS32, DS32
		reg |= (2 << 1); //0b010 = reads 4 bytes per beat
		reg |= (2 << 15); //0b010 = writes 4 bytes per beat

		offs += emit_mov(&chan->ibuf[offs], R_CCR, reg);

		src_addr_lo = (uint32_t)sg[i].src_addr;
		dst_addr_lo = (uint32_t)sg[i].dst_addr;
		len = (uint32_t)sg[i].len;

#if 0
		printf("%s: src %x dst %x len %d periph_id %d\n", __func__,
		    src_addr_lo, dst_addr_lo, len, data->periph_id);
#endif

		offs += emit_mov(&ibuf[offs], R_SAR, src_addr_lo);
		offs += emit_mov(&ibuf[offs], R_DAR, dst_addr_lo);

		cnt = (len / 4);
		if (cnt > 128) {
			offs += emit_lp(&ibuf[offs], 0, cnt / 128);
			offs0 = offs;
			offs += emit_lp(&ibuf[offs], 1, 128);
			offs1 = offs;
		} else {
			offs += emit_lp(&ibuf[offs], 0, cnt);
			offs0 = offs;
		}
		offs += emit_wfp(&ibuf[offs], data->periph_id); //25 -- qspi rx
		offs += emit_ld(&ibuf[offs]);
		offs += emit_st(&ibuf[offs]);

		if (cnt > 128) {
			jump_addr_relative = (offs - offs1);
			offs += emit_lpend(&ibuf[offs], 1, jump_addr_relative);
			jump_addr_relative = (offs - offs0);
			offs += emit_lpend(&ibuf[offs], 0, jump_addr_relative);
		} else {
			jump_addr_relative = (offs - offs0);
			offs += emit_lpend(&ibuf[offs], 0, jump_addr_relative);
		}
	}

	offs += emit_sev(&ibuf[offs], chan->index);
	offs += emit_end(&ibuf[offs]);

	emit_go(dbuf, chan->index, chan->ibuf_phys);

	reg = (dbuf[1] << 24) | (dbuf[0] << 16);
	//reg |= (chan->index << 8);
	WRITE4(sc, DBGINST0, reg);
	reg = (dbuf[5] << 24) | (dbuf[4] << 16) | (dbuf[3] << 8) | dbuf[2];
	WRITE4(sc, DBGINST1, reg);

	WRITE4(sc, INTCLR, 0xffffffff);
	WRITE4(sc, INTEN, (1 << chan->index));

	chan->enqueued = sg_n;
	chan->capacity = 0;

	/* Start operation */
	WRITE4(sc, DBGCMD, 0);

	return (0);
}

static int
pl330_channel_prep_sg(device_t dev, struct xdma_channel *xchan)
{
	struct pl330_channel *chan;
	struct pl330_softc *sc;
	//uint32_t addr;
	//uint32_t reg;
	//int ret;
	//int i;

	sc = device_get_softc(dev);

#if 1
	printf("%s(%d)\n", __func__, device_get_unit(dev));
#endif

	chan = (struct pl330_channel *)xchan->chan;
	chan->capacity = 0x10000;

	return (0);
}

static int
pl330_channel_control(device_t dev, xdma_channel_t *xchan, int cmd)
{
	struct pl330_channel *chan;
	struct pl330_softc *sc;

	sc = device_get_softc(dev);

	chan = (struct pl330_channel *)xchan->chan;

	switch (cmd) {
	case XDMA_CMD_BEGIN:
	case XDMA_CMD_TERMINATE:
	case XDMA_CMD_PAUSE:
		/* TODO: implement me */
		return (-1);
	}

	return (0);
}

#ifdef FDT
static int
pl330_ofw_md_data(device_t dev, pcell_t *cells, int ncells, void **ptr)
{
	struct pl330_fdt_data *data;

	if (ncells != 1) {
		return (-1);
	}

	data = malloc(sizeof(struct pl330_fdt_data),
	    M_DEVBUF, (M_WAITOK | M_ZERO));
	if (data == NULL) {
		device_printf(dev, "%s: Cant allocate memory\n", __func__);
		return (-1);
	}

	data->periph_id = cells[0];

	*ptr = data;

	return (0);
}
#endif

static device_method_t pl330_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe,			pl330_probe),
	DEVMETHOD(device_attach,		pl330_attach),
	DEVMETHOD(device_detach,		pl330_detach),

	/* xDMA Interface */
	DEVMETHOD(xdma_channel_alloc,		pl330_channel_alloc),
	DEVMETHOD(xdma_channel_free,		pl330_channel_free),
	DEVMETHOD(xdma_channel_control,		pl330_channel_control),

	/* xDMA SG Interface */
	DEVMETHOD(xdma_channel_capacity,	pl330_channel_capacity),
	DEVMETHOD(xdma_channel_prep_sg,		pl330_channel_prep_sg),
	DEVMETHOD(xdma_channel_submit_sg,	pl330_channel_submit_sg),

#ifdef FDT
	DEVMETHOD(xdma_ofw_md_data,		pl330_ofw_md_data),
#endif

	DEVMETHOD_END
};

static driver_t pl330_driver = {
	"pl330",
	pl330_methods,
	sizeof(struct pl330_softc),
};

static devclass_t pl330_devclass;

EARLY_DRIVER_MODULE(pl330, simplebus, pl330_driver, pl330_devclass, 0, 0,
    BUS_PASS_INTERRUPT + BUS_PASS_ORDER_LATE);
