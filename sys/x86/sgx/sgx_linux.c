/*-
 * Copyright (c) 2017 Ruslan Bukin <br@bsdpad.com>
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
#include <sys/capsicum.h>
#include <sys/conf.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/file.h>
#include <sys/proc.h>
#ifdef __amd64__
#include <machine/../linux32/linux.h>
#include <machine/../linux32/linux32_proto.h>
#else
#include <machine/../linux/linux.h>
#include <machine/../linux/linux_proto.h>
#endif
#include <compat/linux/linux_ioctl.h>

static int
sgx_linux_modevent(module_t mod, int type, void *data)
{

	return (0);
}

DEV_MODULE(sgx_linux, sgx_linux_modevent, NULL);
MODULE_DEPEND(sgx_linux, linux64, 1, 1, 1);

static int
sgx_linux_ioctl(struct thread *td, struct linux_ioctl_args *args)
{
	cap_rights_t rights;
	struct file *fp;
	u_long cmd;
	int error;

	error = fget(td, args->fd, cap_rights_init(&rights, CAP_IOCTL), &fp);
	if (error != 0)
		return (error);
	cmd = args->cmd;

	/*
	 * Pass the ioctl off to our standard handler.
	 */
	error = (fo_ioctl(fp, cmd, (caddr_t)args->arg, td->td_ucred, td));
	fdrop(fp, td);

	return (error);
}

/* There are multiple ioctl number ranges that need to be handled */
#define SGX_LINUX_IOCTL_MIN  0xa400
#define SGX_LINUX_IOCTL_MAX  0xa402

static linux_ioctl_function_t sgx_linux_ioctl;
static struct linux_ioctl_handler sgx_linux_handler = {
	sgx_linux_ioctl,
	SGX_LINUX_IOCTL_MIN,
	SGX_LINUX_IOCTL_MAX,
};

SYSINIT(sgx_linux_register, SI_SUB_KLD, SI_ORDER_MIDDLE,
    linux_ioctl_register_handler, &sgx_linux_handler);
SYSUNINIT(sgx_linux_unregister, SI_SUB_KLD, SI_ORDER_MIDDLE,
    linux_ioctl_unregister_handler, &sgx_linux_handler);
