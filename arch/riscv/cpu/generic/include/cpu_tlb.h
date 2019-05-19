/**
 * Copyright (c) 2019 Western Digital Corporation or its affiliates.
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * @file cpu_tlb.h
 * @author Anup Patel (anup.patel@wdc.com)
 * @brief RISC-V CPU TLB functions
 */

#ifndef _CPU_TLB_H__
#define _CPU_TLB_H__

#include <vmm_types.h>

/** Invalidate Stage2 TLBs for given VMID and guest physical address */
void __hfence_gvma_vmid_gpa(unsigned long vmid, unsigned long gpa);

/** Invalidate Stage2 TLBs for given VMID */
void __hfence_gvma_vmid(unsigned long vmid);

/** Invalidate Stage2 TLBs for given guest physical address */
void __hfence_gvma_gpa(unsigned long gpa);

/** Invalidate all possible Stage2 TLBs */
void __hfence_gvma_all(void);

#endif
