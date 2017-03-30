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

/* Cadence Quad SPI Flash Controller driver. */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include "opt_platform.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bio.h>
#include <sys/bus.h>
#include <sys/conf.h>
#include <sys/kernel.h>
#include <sys/kthread.h>
#include <sys/lock.h>
#include <sys/mbuf.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <geom/geom_disk.h>

#include <machine/bus.h>

#include <dev/fdt/simplebus.h>
#include <dev/fdt/fdt_common.h>
#include <dev/ofw/ofw_bus_subr.h>
#include <dev/ofw/openfirm.h>

#include <dev/flash/cqspi.h>
#include <dev/xdma/xdma.h>

#include "qspi_if.h"

#define	CQSPI_SECTORSIZE	512
#define	TX_QUEUE_SIZE		16
#define	RX_QUEUE_SIZE		16

#define	READ4(_sc, _reg) bus_read_4((_sc)->res[0], _reg)
#define READ2(_sc, _reg) bus_read_2((_sc)->res[0], _reg)
#define READ1(_sc, _reg) bus_read_1((_sc)->res[0], _reg)
#define WRITE4(_sc, _reg, _val) bus_write_4((_sc)->res[0], _reg, _val)
#define WRITE2(_sc, _reg, _val) bus_write_2((_sc)->res[0], _reg, _val)
#define WRITE1(_sc, _reg, _val) bus_write_1((_sc)->res[0], _reg, _val)
#define READ_DATA_4(_sc, _reg) bus_read_4((_sc)->res[1], _reg)
#define READ_DATA_1(_sc, _reg) bus_read_1((_sc)->res[1], _reg)
#define WRITE_DATA_4(_sc, _reg, _val) bus_write_4((_sc)->res[1], _reg, _val)
#define WRITE_DATA_1(_sc, _reg, _val) bus_write_1((_sc)->res[1], _reg, _val)

struct cqspi_softc {
	device_t	dev;
	uint8_t		sc_manufacturer_id;
	uint16_t	device_id;
	unsigned int	sc_sectorsize;
	struct mtx	sc_mtx;
	struct disk	*sc_disk;
	struct proc	*sc_p;
	struct bio_queue_head sc_bio_queue;
	unsigned int	sc_flags;

	struct resource		*res[3];
	bus_space_tag_t		bst;
	bus_space_handle_t	bsh;
	void			*ih;
	uint8_t			op_done;
	uint8_t			write_op_done;

	/* xDMA */
	xdma_controller_t	*xdma_tx;
	xdma_channel_t		*xchan_tx;
	void			*ih_tx;

	xdma_controller_t	*xdma_rx;
	xdma_channel_t		*xchan_rx;
	void			*ih_rx;
};

#define	CQSPI_LOCK(_sc)		mtx_lock(&(_sc)->sc_mtx)
#define	CQSPI_UNLOCK(_sc)	mtx_unlock(&(_sc)->sc_mtx)
#define CQSPI_LOCK_INIT(_sc)					\
	mtx_init(&_sc->sc_mtx, device_get_nameunit(_sc->dev),	\
	    "cqspi", MTX_DEF)
#define CQSPI_LOCK_DESTROY(_sc)	mtx_destroy(&_sc->sc_mtx);
#define CQSPI_ASSERT_LOCKED(_sc)				\
	mtx_assert(&_sc->sc_mtx, MA_OWNED);
#define CQSPI_ASSERT_UNLOCKED(_sc)				\
	mtx_assert(&_sc->sc_mtx, MA_NOTOWNED);

static struct resource_spec cqspi_spec[] = {
	{ SYS_RES_MEMORY,	0,	RF_ACTIVE },
	{ SYS_RES_MEMORY,	1,	RF_ACTIVE },
	{ SYS_RES_IRQ,		0,	RF_ACTIVE },
	{ -1, 0 }
};

static struct ofw_compat_data compat_data[] = {
	{ "cdns,qspi-nor",	1 },
	{ NULL,			0 },
};

static void
cqspi_intr(void *arg)
{
	struct cqspi_softc *sc;
	uint32_t pending;

	sc = arg;

	pending = READ4(sc, CQSPI_IRQSTAT);

#if 1
	printf("%s: IRQSTAT %x\n", __func__, pending);
#endif
	if (pending & (IRQMASK_INDOPDONE | IRQMASK_INDXFRLVL | IRQMASK_INDSRAMFULL)) {
		//printf("op_done\n");
		sc->op_done = 1;
	}
	WRITE4(sc, CQSPI_IRQSTAT, pending);
}

static int
cqspi_xdma_tx_intr(void *arg, xdma_transfer_status_t *status)
{
	struct cqspi_softc *sc;
	struct bio *bp;
	int ret;
	struct xdma_transfer_status st;

	sc = arg;

	//printf("%s\n", __func__);

	while (1) {
		//ret = xdma_dequeue(sc->xchan_tx, (void **)&bp, &st);
		ret = xdma_dequeue_bio(sc->xchan_tx, &bp, &st);
		if (ret != 0) {
			break;
		}
		//printf("bio done: %x\n", (uint32_t)bp);
		//printf(".");
		//biodone(bp);
		sc->write_op_done = 1;
	}

	wakeup(&sc->xdma_tx);

	return (0);
}

static int
cqspi_xdma_rx_intr(void *arg, xdma_transfer_status_t *status)
{
	struct cqspi_softc *sc;
	struct bio *bp;
	int ret;
	struct xdma_transfer_status st;

	sc = arg;

	//printf("%s\n", __func__);

	while (1) {
		//ret = xdma_dequeue(sc->xchan_rx, (void **)&bp, &st);
		ret = xdma_dequeue_bio(sc->xchan_rx, &bp, &st);
		if (ret != 0) {
			break;
		}
		//printf("bio done: %x\n", (uint32_t)bp);
		//printf(".");
		//biodone(bp);
		sc->op_done = 1;
	}

	//CQSPI_LOCK(sc);
	wakeup(&sc->xdma_rx);
	//CQSPI_UNLOCK(sc);

	return (0);
}

static int
cqspi_cmd_write_reg(struct cqspi_softc *sc, uint8_t cmd, uint32_t addr, uint32_t len)
{
	uint32_t reg;
	int timeout;
	int i;

	reg = (cmd << FLASHCMD_CMDOPCODE_S);
	WRITE4(sc, CQSPI_FLASHCMD, reg);
	reg |= FLASHCMD_EXECCMD;
	WRITE4(sc, CQSPI_FLASHCMD, reg);

	timeout = 1000;
	for (i = timeout; i > 0; i--) {
		if ((READ4(sc, CQSPI_FLASHCMD) & FLASHCMD_CMDEXECSTAT) == 0) {
			break;
		}
	}
	if (i == 0) {
		printf("%s: cmd timed out\n", __func__);
	}

	return (0);
}

static int
cqspi_cmd_write(struct cqspi_softc *sc, uint8_t cmd, uint32_t addr, uint32_t len)
{
	uint32_t reg;
	int timeout;
	int i;

	//printf("%s: %x\n", __func__, cmd);

	WRITE4(sc, CQSPI_FLASHCMDADDR, addr);
	reg = (cmd << FLASHCMD_CMDOPCODE_S);
	reg |= (FLASHCMD_ENCMDADDR);
	reg |= ((len - 1) << FLASHCMD_NUMADDRBYTES_S);
	WRITE4(sc, CQSPI_FLASHCMD, reg);

	reg |= FLASHCMD_EXECCMD;
	WRITE4(sc, CQSPI_FLASHCMD, reg);

	timeout = 1000;
	for (i = timeout; i > 0; i--) {
		if ((READ4(sc, CQSPI_FLASHCMD) & FLASHCMD_CMDEXECSTAT) == 0) {
			break;
		}
	}
	if (i == 0) {
		printf("%s: cmd timed out\n", __func__);
	}

	return (0);
}

static int
cqspi_cmd(struct cqspi_softc *sc, uint8_t cmd, uint32_t len)
{
	uint32_t data;
	uint32_t reg;
	int timeout;
	int i;

	//printf("%s: %x\n", __func__, cmd);
	//printf("datardlo before %x\n", READ4(sc, CQSPI_FLASHCMDRDDATALO));

	reg = (cmd << FLASHCMD_CMDOPCODE_S);
	reg |= ((len - 1) << FLASHCMD_NUMRDDATABYTES_S);
	reg |= FLASHCMD_ENRDDATA;
	WRITE4(sc, CQSPI_FLASHCMD, reg);

	reg |= FLASHCMD_EXECCMD;
	WRITE4(sc, CQSPI_FLASHCMD, reg);

	timeout = 10000;
	for (i = timeout; i > 0; i--) {
		if ((READ4(sc, CQSPI_FLASHCMD) & FLASHCMD_CMDEXECSTAT) == 0) {
			break;
		}
	}
	if (i == 0) {
		printf("%s: cmd %x timed out\n", __func__, cmd);
		return (0);
	}

	//printf("i %d\n", i);
	//printf("cmd %x\n", READ4(sc, CQSPI_FLASHCMD));
	//printf("datardlo %x\n", READ4(sc, CQSPI_FLASHCMDRDDATALO));
	//printf("datardup %x\n", READ4(sc, CQSPI_FLASHCMDRDDATAUP));

	data = READ4(sc, CQSPI_FLASHCMDRDDATALO);

	switch (len) {
	case 4:
		return (data);
	case 3:
		return (data & 0xffffff);
	case 2:
		return (data & 0xffff);
	case 1:
		return (data & 0xff);
	default:
		return (0);
	}

	return (0);
}

static int
cqspi_wait_ready(struct cqspi_softc *sc)
{
	uint8_t reg;

	do {
		reg = cqspi_cmd(sc, CMD_READ_STATUS, 1);
	} while (reg & (1 << 0));

	return (0);
}

static int
cqspi_write(device_t dev, device_t child, struct bio *bp,
    off_t offset, caddr_t data, off_t count)
{
	struct cqspi_softc *sc;
	device_t pdev;
	uint32_t reg;

	pdev = device_get_parent(dev);
	sc = device_get_softc(dev);

	cqspi_wait_ready(sc);
	//printf("%s: status %x\n", __func__, cqspi_cmd(sc, CMD_READ_STATUS, 1));

	reg = cqspi_cmd_write_reg(sc, CMD_WRITE_ENABLE, 0, 0);

	cqspi_wait_ready(sc);
	//printf("%s: status %x\n", __func__, cqspi_cmd(sc, CMD_READ_STATUS, 1));

	reg = cqspi_cmd_write(sc, 0xdc, offset, 4);

	cqspi_wait_ready(sc);
	//printf("%s: status %x\n", __func__, cqspi_cmd(sc, CMD_READ_STATUS, 1));

	reg = cqspi_cmd_write_reg(sc, CMD_WRITE_ENABLE, 0, 0);

	cqspi_wait_ready(sc);
	//printf("%s: status %x\n", __func__, cqspi_cmd(sc, CMD_READ_STATUS, 1));

	sc->write_op_done = 0;

	//printf("%s: offset 0x%llx count %lld bytes\n", __func__, offset, count);

	reg = (2 << 0); //numsglreqbytes
	reg |= (2 << 8); //numburstreqbytes
	WRITE4(sc, CQSPI_DMAPER, reg);

	WRITE4(sc, CQSPI_INDWRWATER, 64); //(128 * 4) / 8);

	WRITE4(sc, CQSPI_INDWR, INDRD_IND_OPS_DONE_STATUS);
	WRITE4(sc, CQSPI_INDWR, 0);

	WRITE4(sc, CQSPI_INDWRCNT, count);
	WRITE4(sc, CQSPI_INDWRSTADDR, offset);

#define	QUAD_INPUT_FAST_PROGRAM	0x32

	reg = (0 << DEVRD_DUMMYRDCLKS_S);
	reg |= (2 << 16); //data width
	reg |= (0 << 12); //addr width

	//dont use this
	//reg |= (QUAD_INPUT_FAST_PROGRAM << DEVRD_RDOPCODE_S);

	reg |= (0x34 << DEVRD_RDOPCODE_S);
	WRITE4(sc, CQSPI_DEVWR, reg);

	reg = (2 << 16); //data width
	reg |= (0 << 12); //addr width
	reg |= (0 <<  8); //inst width
	WRITE4(sc, CQSPI_DEVRD, reg);

	WRITE4(sc, CQSPI_MODEBIT, 0);

#if 1
	xdma_enqueue_bio(sc->xchan_tx, &bp, 0xffa00000, XDMA_MEM_TO_DEV);
	xdma_queue_submit(sc->xchan_tx);
#endif

	WRITE4(sc, CQSPI_INDWR, INDRD_START);

#if 0
	WRITE_DATA_4(sc, 0, 0);
	uint32_t fill;
	for (i = 0; i < 512/4; i++) {
		fill = READ4(sc, CQSPI_SRAMFILL);
		fill >>= 16;
		fill &= 0xffff;
		printf("fill %d\n", fill);
		//WRITE_DATA_4(sc, 0, 0);
	}

	for (i = 0; i < 10; i++) {
		printf("indwr %x\n", READ4(sc, CQSPI_INDWR));
		DELAY(10000);
	}
#endif

	CQSPI_LOCK(sc);
	while (sc->write_op_done == 0) {
		tsleep(&sc->xdma_tx, PCATCH | PZERO, "spi", hz/2);
	}
	CQSPI_UNLOCK(sc);

	return (0);
}

static int
cqspi_read(device_t dev, device_t child, struct bio *bp,
    off_t offset, caddr_t data, off_t count)
{
	struct cqspi_softc *sc;
	device_t pdev;
	uint32_t reg;

	pdev = device_get_parent(dev);
	sc = device_get_softc(dev);

	//printf("%s: offset 0x%llx count %lld bytes\n", __func__, offset, count);
	sc->op_done = 0;

	reg = (2 << 0); //numsglreqbytes
	reg |= (2 << 8); //numburstreqbytes
	WRITE4(sc, CQSPI_DMAPER, reg);

	WRITE4(sc, CQSPI_INDRDWATER, 64);
	WRITE4(sc, CQSPI_INDRD, INDRD_IND_OPS_DONE_STATUS);
	WRITE4(sc, CQSPI_INDRD, 0);

	WRITE4(sc, CQSPI_INDRDCNT, count);
	WRITE4(sc, CQSPI_INDRDSTADDR, offset);

	reg = (0 << DEVRD_DUMMYRDCLKS_S);
	reg |= (2 << 16); //data width
	reg |= (0 << 12); //addr width
	reg |= (0 <<  8); //inst width
	reg |= (1 << 20); //enmodebits
	reg |= (CMD_READ_4B_QUAD_OUTPUT << DEVRD_RDOPCODE_S);
	WRITE4(sc, CQSPI_DEVRD, reg);

	WRITE4(sc, CQSPI_MODEBIT, 0xff);

	reg = READ4(sc, CQSPI_IRQMASK);
	reg |= (IRQMASK_INDOPDONE | IRQMASK_INDXFRLVL | IRQMASK_INDSRAMFULL);
	//WRITE4(sc, CQSPI_IRQMASK, reg);

	sc->op_done = 0;

#if 0
	uint32_t *addr;
	int i;
	int n;
	uint32_t cnt;
	addr = (uint32_t *)data;

	WRITE4(sc, CQSPI_INDRD, INDRD_START);

	n = 0;
	while (n < (count / 4)) {
		cnt = READ4(sc, CQSPI_SRAMFILL) & 0xffff;
		for (i = 0; i < cnt; i++) {
			addr[n++] = READ_DATA_4(sc, 0);
		}
	}

	while ((READ4(sc, CQSPI_INDRD) & INDRD_IND_OPS_DONE_STATUS) == 0)
		;

	WRITE4(sc, CQSPI_INDRD, INDRD_IND_OPS_DONE_STATUS);
	WRITE4(sc, CQSPI_IRQSTAT, 0);
#else
	//printf("enqueing bp %p\n", &bp);
	//xdma_enqueue(sc->xchan_rx, 0xffa00000, (uintptr_t)data, count, XDMA_DEV_TO_MEM, bp);
	xdma_enqueue_bio(sc->xchan_rx, &bp, 0xffa00000, XDMA_DEV_TO_MEM);
	xdma_queue_submit(sc->xchan_rx);

	WRITE4(sc, CQSPI_INDRD, INDRD_START);
#endif

	CQSPI_LOCK(sc);
	//printf("sl\n");
	while (sc->op_done == 0) {
		//mtx_sleep(sc->xdma_rx, &sc->sc_mtx, PRIBIO, "job queue", hz/2);
		tsleep(&sc->xdma_rx, PCATCH | PZERO, "spi", hz/2);
	}
	//printf("sd\n");
	CQSPI_UNLOCK(sc);

	return (0);
}

static int
cqspi_init(struct cqspi_softc *sc)
{
	uint32_t reg;

	printf("Module ID %x\n", READ4(sc, CQSPI_MODULEID));
	printf("cfg %x\n", READ4(sc, CQSPI_CFG));

	/* Disable controller */
	reg = READ4(sc, CQSPI_CFG);
	reg &= ~(CFG_EN);
	WRITE4(sc, CQSPI_CFG, reg);

	reg = READ4(sc, CQSPI_DEVSZ);
	reg |= 3;
	WRITE4(sc, CQSPI_DEVSZ, reg);
	printf("devsz %x\n", reg);

	WRITE4(sc, CQSPI_SRAMPART, 128/2);

	reg = READ4(sc, CQSPI_CFG);
	/* Configure baud rate */
	reg &= ~(CFG_BAUD_M);
	reg |= CFG_BAUD4;
	reg |= CFG_ENDMA;
	WRITE4(sc, CQSPI_CFG, reg);

	reg = (3 << DELAY_NSS_S);
	reg |= (3 << DELAY_BTWN_S);
	reg |= (1 << DELAY_AFTER_S);
	reg |= (1 << DELAY_INIT_S);

	reg = (3 << DELAY_NSS_S);
	reg |= (3  << DELAY_BTWN_S);
	reg |= (1 << DELAY_AFTER_S);
	reg |= (1 << DELAY_INIT_S);
	WRITE4(sc, CQSPI_DELAY, reg);

	READ4(sc, CQSPI_RDDATACAP);
	reg &= ~(RDDATACAP_DELAY_M);
	reg |= (1 << RDDATACAP_DELAY_S);
	WRITE4(sc, CQSPI_RDDATACAP, reg);

	/* Enable controller */
	reg = READ4(sc, CQSPI_CFG);
	reg |= (CFG_EN);
	WRITE4(sc, CQSPI_CFG, reg);

	reg = cqspi_cmd(sc, CMD_READ_IDENT, 4);
	printf("Ident %x\n", reg);

	reg = cqspi_cmd(sc, CMD_READ_STATUS, 1);
	printf("Status %x\n", reg);

	printf("Enter 4b mode\n");
	cqspi_cmd(sc, CMD_ENTER_4B_MODE, 1);

	//printf("Exit 4b mode\n");
	//cqspi_cmd(sc, CMD_EXIT_4B_MODE, 1);

	reg = cqspi_cmd(sc, CMD_READ_NVCONF_REG, 2);
	printf("NVCONF %x\n", reg);

	reg = cqspi_cmd(sc, CMD_READ_CONF_REG, 1);
	printf("CONF %x\n", reg);

	reg = cqspi_cmd(sc, CMD_READ_FSR, 1);
	printf("FSR %x\n", reg);

	return (0);
}

static int
cqspi_add_devices(device_t dev)
{
	phandle_t child, node;
	device_t child_dev;
	int error;

	node = ofw_bus_get_node(dev);
	simplebus_init(dev, node);
	for (child = OF_child(node); child != 0; child = OF_peer(child)) {
		child_dev = simplebus_add_device(dev, child, 0, NULL, -1, NULL);
		if (child_dev == NULL) {
			return (ENXIO);
		}

		error = device_probe_and_attach(child_dev);
		if (error != 0) {
			printf("can't probe and attach: %d\n", error);
		}
	}

	return (0);
}

static int
cqspi_probe(device_t dev)
{

	if (!ofw_bus_status_okay(dev)) {
		return (ENXIO);
	}

	if (!ofw_bus_search_compatible(dev, compat_data)->ocd_data) {
		return (ENXIO);
	}

	device_set_desc(dev, "Cadence Quad SPI controller");

	return (0);
}

static int
cqspi_attach(device_t dev)
{
	struct cqspi_softc *sc;
	uint32_t caps;
	int error;

	sc = device_get_softc(dev);
	sc->dev = dev;

	if (bus_alloc_resources(dev, cqspi_spec, sc->res)) {
		device_printf(dev, "could not allocate resources\n");
		return (ENXIO);
	}

	/* Memory interface */
	sc->bst = rman_get_bustag(sc->res[0]);
	sc->bsh = rman_get_bushandle(sc->res[0]);

	/* Setup interrupt handlers */
	if (bus_setup_intr(sc->dev, sc->res[2], INTR_TYPE_BIO | INTR_MPSAFE,
	    NULL, cqspi_intr, sc, &sc->ih)) {
		device_printf(sc->dev, "Unable to setup intr\n");
		return (ENXIO);
	}

	CQSPI_LOCK_INIT(sc);

	caps = 0;

	/* Get xDMA controller. */
	sc->xdma_tx = xdma_ofw_get(sc->dev, "tx");
	if (sc->xdma_tx == NULL) {
		device_printf(dev, "Can't find DMA controller.\n");
		return (ENXIO);
	}

	sc->xdma_rx = xdma_ofw_get(sc->dev, "rx");
	if (sc->xdma_rx == NULL) {
		device_printf(dev, "Can't find DMA controller.\n");
		return (ENXIO);
	}

	/* Alloc xDMA virtual channels. */
	sc->xchan_tx = xdma_channel_alloc(sc->xdma_tx, caps);
	if (sc->xchan_tx == NULL) {
		device_printf(dev, "Can't alloc virtual DMA channel.\n");
		return (ENXIO);
	}

	sc->xchan_rx = xdma_channel_alloc(sc->xdma_rx, caps);
	if (sc->xchan_rx == NULL) {
		device_printf(dev, "Can't alloc virtual DMA channel.\n");
		return (ENXIO);
	}

	/* Setup xDMA interrupt handlers. */
	error = xdma_setup_intr(sc->xchan_tx, cqspi_xdma_tx_intr, sc, &sc->ih_tx);
	if (error) {
		device_printf(sc->dev,
		    "Can't setup xDMA interrupt handler.\n");
		return (ENXIO);
	}

	error = xdma_setup_intr(sc->xchan_rx, cqspi_xdma_rx_intr, sc, &sc->ih_rx);
	if (error) {
		device_printf(sc->dev,
		    "Can't setup xDMA interrupt handler.\n");
		return (ENXIO);
	}

	xdma_prep_sg(sc->xchan_tx, TX_QUEUE_SIZE, MAXPHYS, 16);
	xdma_prep_sg(sc->xchan_rx, TX_QUEUE_SIZE, MAXPHYS, 16);

	cqspi_init(sc);
	cqspi_add_devices(dev);

	return (bus_generic_attach(dev));
}

static int
cqspi_detach(device_t dev)
{

	return (EIO);
}

static device_method_t cqspi_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe,		cqspi_probe),
	DEVMETHOD(device_attach,	cqspi_attach),
	DEVMETHOD(device_detach,	cqspi_detach),

	DEVMETHOD(qspi_read,		cqspi_read),
	DEVMETHOD(qspi_write,		cqspi_write),
	{ 0, 0 }
};

DEFINE_CLASS_1(cqspi, cqspi_driver, cqspi_methods,
    sizeof(struct cqspi_softc), simplebus_driver);

static devclass_t cqspi_devclass;

DRIVER_MODULE(cqspi, simplebus, cqspi_driver, cqspi_devclass, 0, 0);
