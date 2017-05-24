/*
 * (C) Copyright 2016 Intel Corporation
 *
 * Authors:
 *
 * Jarkko Sakkinen <jarkko.sakkinen@intel.com>
 * Suresh Siddha <suresh.b.siddha@intel.com>
 * Serge Ayoun <serge.ayoun@intel.com>
 * Shay Katz-zamir <shay.katz-zamir@intel.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2
 * of the License.
 */

#ifndef _UAPI_ASM_X86_SGX_H
#define _UAPI_ASM_X86_SGX_H

//#include <linux/types.h>
//#include <linux/ioctl.h>

#define SGX_MAGIC 0xA4

#define SGX_IOC_ENCLAVE_CREATE \
	_IOW(SGX_MAGIC, 0x00, struct sgx_enclave_create)
#define SGX_IOC_ENCLAVE_ADD_PAGE \
	_IOW(SGX_MAGIC, 0x01, struct sgx_enclave_add_page)
#define SGX_IOC_ENCLAVE_INIT \
	_IOW(SGX_MAGIC, 0x02, struct sgx_enclave_init)

/* SGX leaf instruction return values */
#define SGX_SUCCESS			0
#define SGX_INVALID_SIG_STRUCT		1
#define SGX_INVALID_ATTRIBUTE		2
#define SGX_BLKSTATE			3
#define SGX_INVALID_MEASUREMENT		4
#define SGX_NOTBLOCKABLE		5
#define SGX_PG_INVLD			6
#define SGX_LOCKFAIL			7
#define SGX_INVALID_SIGNATURE		8
#define SGX_MAC_COMPARE_FAIL		9
#define SGX_PAGE_NOT_BLOCKED		10
#define SGX_NOT_TRACKED			11
#define SGX_VA_SLOT_OCCUPIED		12
#define SGX_CHILD_PRESENT		13
#define SGX_ENCLAVE_ACT			14
#define SGX_ENTRYEPOCH_LOCKED		15
#define SGX_INVALID_LICENSE		16
#define SGX_PREV_TRK_INCMPL		17
#define SGX_PG_IS_SECS			18
#define SGX_INVALID_CPUSVN		32
#define SGX_INVALID_ISVSVN		64
#define SGX_UNMASKED_EVENT		128
#define SGX_INVALID_KEYNAME		256

/* IOCTL return values */
#define SGX_POWER_LOST_ENCLAVE		0x40000000
#define SGX_LE_ROLLBACK			0x40000001

struct sgx_secinfo {
	uint64_t flags;
	uint64_t reserved[7];
} __attribute__((aligned(128)));

struct sgx_enclave_create {
	uint64_t	src;
} __attribute__((packed));

struct sgx_enclave_add_page {
	uint64_t	addr;
	uint64_t	src;
	uint64_t	secinfo;
	uint16_t	mrmask;
} __attribute__((packed));

struct sgx_enclave_init {
	uint64_t	addr;
	uint64_t	sigstruct;
	uint64_t	einittoken;
} __attribute__((packed));

/*
 * 2.7 SGX ENCLAVE CONTROL STRUCTURE (SECS)
 * The SECS data structure requires 4K-Bytes alignment.
 */

struct secs_attr {
	uint8_t reserved1: 1;
	uint8_t debug: 1;
	uint8_t mode64bit: 1;
	uint8_t reserved2: 1;
	uint8_t provisionkey: 1;
	uint8_t einittokenkey: 1;
	uint8_t reserved3: 2;
	uint8_t reserved4[7];
	uint64_t xfrm;			/* X-Feature Request Mask */
};

struct secs {
	uint64_t	size;
	uint64_t	base;
	uint32_t	ssa_frame_size;
	uint32_t	misc_select;
	uint8_t		reserved1[24];
	struct secs_attr attributes;
	uint8_t		mr_enclave[32];
	uint8_t		reserved2[32];
	uint8_t		mr_signer[32];
	uint8_t		reserved3[96];
	uint16_t	isv_prod_id;
	uint16_t	isv_svn;
	uint8_t		reserved4[3836];
};

struct tcs {
	uint64_t state;
	uint64_t flags;
	uint64_t ossa;
	uint32_t cssa;
	uint32_t nssa;
	uint64_t oentry;
	uint64_t aep;
	uint64_t ofsbasgx;
	uint64_t ogsbasgx;
	uint32_t fslimit;
	uint32_t gslimit;
	uint64_t reserved[503];
};

#endif /* _UAPI_ASM_X86_SGX_H */
