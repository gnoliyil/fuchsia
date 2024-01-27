/******************************************************************************
  SPDX-License-Identifier: BSD-3-Clause

  Copyright (c) 2001-2015, Intel Corporation
  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are met:

   1. Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.

   2. Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.

   3. Neither the name of the Intel Corporation nor the names of its
      contributors may be used to endorse or promote products derived from
      this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
  POSSIBILITY OF SUCH DAMAGE.

******************************************************************************/
/*$FreeBSD$*/

#ifndef ZIRCON_THIRD_PARTY_DEV_ETHERNET_E1000_E1000_OSDEP_H_
#define ZIRCON_THIRD_PARTY_DEV_ETHERNET_E1000_E1000_OSDEP_H_

#include <assert.h>
#include <fuchsia/hardware/ethernet/c/banjo.h>
#include <inttypes.h>
#include <lib/ddk/debug.h>
#include <lib/ddk/device.h>
#include <lib/ddk/driver.h>
#include <lib/ddk/hw/inout.h>
#include <lib/ddk/hw/reg.h>
#include <lib/ddk/io-buffer.h>
#include <lib/device-protocol/pci.h>
#include <lib/mmio/mmio-buffer.h>
#include <lib/pci/hw.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <zircon/assert.h>
#include <zircon/syscalls.h>
#include <zircon/types.h>

#define ASSERT(x) assert(x)

#define nsec_delay(x) zx_nanosleep(zx_deadline_after(x))
#define usec_delay(x) nsec_delay(ZX_USEC(x))
#define usec_delay_irq(x) nsec_delay(ZX_USEC(x))
#define msec_delay(x) nsec_delay(ZX_MSEC(x))
#define msec_delay_irq(x) nsec_delay(ZX_MSEC(x))

/* Enable/disable debugging statements in shared code */
#define DEBUGOUT(format, ...) zxlogf(DEBUG, "%s %d: " format, __FUNCTION__, __LINE__, ##__VA_ARGS__)
#define DEBUGOUT1(...) DEBUGOUT(__VA_ARGS__)
#define DEBUGOUT2(...) DEBUGOUT(__VA_ARGS__)
#define DEBUGOUT3(...) DEBUGOUT(__VA_ARGS__)
#define DEBUGFUNC(F) zxlogf(DEBUG, F "")

#define STATIC static
#define FALSE 0
#define TRUE 1

#define CMD_MEM_WRT_INVALIDATE PCI_COMMAND_MEM_WR_INV_EN /* BIT_4 */
#define PCI_COMMAND_REGISTER PCIR_COMMAND

typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;
typedef int64_t s64;
typedef int32_t s32;
typedef int16_t s16;
typedef int8_t s8;

#define __le16 u16
#define __le32 u32
#define __le64 u64

#define ASSERT_CTX_LOCK_HELD(hw)

// An opaque struct that contains a C++ PCI client.
struct e1000_pci;

#if defined(__cplusplus)
extern "C" {
#endif  // defined(__cplusplus)

// A set of C wrappers around the C++ PCI methods.
zx_status_t e1000_pci_set_bus_mastering(const struct e1000_pci* pci, bool enabled);
zx_status_t e1000_pci_ack_interrupt(const struct e1000_pci* pci);
zx_status_t e1000_pci_read_config16(const struct e1000_pci* pci, uint16_t offset,
                                    uint16_t* out_value);
zx_status_t e1000_pci_get_device_info(const struct e1000_pci* pci, pci_device_info_t* out_info);
zx_status_t e1000_pci_map_bar_buffer(const struct e1000_pci* pci, uint32_t bar_id,
                                     uint32_t cache_policy, void* mmio);
zx_status_t e1000_pci_get_bar(const struct e1000_pci* pci, uint32_t bar_id, pci_bar_t* out_result);
zx_status_t e1000_pci_get_bti(const struct e1000_pci* pci, uint32_t index, zx_handle_t* out_bti);
zx_status_t e1000_pci_configure_interrupt_mode(const struct e1000_pci* pci,
                                               uint32_t requested_irq_count,
                                               pci_interrupt_mode_t* out_mode);
zx_status_t e1000_pci_map_interrupt(const struct e1000_pci* pci, uint32_t which_irq,
                                    zx_handle_t* out_interrupt);

zx_status_t e1000_pci_connect_fragment_protocol(struct zx_device* parent, const char* fragment_name,
                                                struct e1000_pci** pci);
void e1000_pci_free(struct e1000_pci* pci);

bool e1000_pci_is_valid(const struct e1000_pci* pci);

#if defined(__cplusplus)
}  // extern "C"
#endif  // defined(__cplusplus)

struct e1000_osdep {
  struct e1000_pci* pci;
  uintptr_t membase;
  uintptr_t iobase;
  uintptr_t flashbase;
};

#define hw2pci(hw) ((struct e1000_osdep*)(hw)->back)->pci
#define hw2membase(hw) (((struct e1000_osdep*)(hw)->back)->membase)
#define hw2iobase(hw) (((struct e1000_osdep*)(hw)->back)->iobase)
#define hw2flashbase(hw) (((struct e1000_osdep*)(hw)->back)->flashbase)

#define e1000_writeb(v, a) writeb((v), (volatile void*)(uintptr_t)(a))
#define e1000_writew(v, a) writew((v), (volatile void*)(uintptr_t)(a))
#define e1000_writel(v, a) writel((v), (volatile void*)(uintptr_t)(a))
#define e1000_writell(v, a) writell((v), (volatile void*)(uintptr_t)(a))

#define e1000_readb(a) readb((const volatile void*)(uintptr_t)(a))
#define e1000_readw(a) readw((const volatile void*)(uintptr_t)(a))
#define e1000_readl(a) readl((const volatile void*)(uintptr_t)(a))
#define e1000_readll(a) readll((const volatile void*)(uintptr_t)(a))

#define E1000_REGISTER(hw, reg) \
  (((hw)->mac.type >= e1000_82543) ? (u32)(reg) : e1000_translate_register_82542(reg))

#define E1000_WRITE_FLUSH(a) E1000_READ_REG(a, E1000_STATUS)

/* Read from an absolute offset in the adapter's memory space */
#define E1000_READ_OFFSET(hw, offset) e1000_readl(hw2membase(hw) + (offset))

/* Write to an absolute offset in the adapter's memory space */
#define E1000_WRITE_OFFSET(hw, offset, value) e1000_writel((value), hw2membase(hw) + (offset))

/* Register READ/WRITE macros */
#define E1000_READ_REG(hw, reg) E1000_READ_OFFSET((hw), E1000_REGISTER((hw), (reg)))

#define E1000_WRITE_REG(hw, reg, value) \
  E1000_WRITE_OFFSET((hw), E1000_REGISTER((hw), (reg)), (value))

#define E1000_READ_REG_ARRAY(hw, reg, index) \
  E1000_READ_OFFSET((hw), E1000_REGISTER((hw), (reg)) + ((index) << 2))

#define E1000_WRITE_REG_ARRAY(hw, reg, index, value) \
  E1000_WRITE_OFFSET((hw), E1000_REGISTER((hw), (reg)) + ((index) << 2), (value))

#define E1000_READ_REG_ARRAY_DWORD E1000_READ_REG_ARRAY
#define E1000_WRITE_REG_ARRAY_DWORD E1000_WRITE_REG_ARRAY

#define E1000_READ_REG_ARRAY_BYTE(hw, reg, index) \
  e1000_readb(hw2membase(hw) + E1000_REGISTER((hw), (reg)) + (index))

#define E1000_WRITE_REG_ARRAY_BYTE(hw, reg, index, value) \
  e1000_writeb((value), hw2membase(hw) + E1000_REGISTER((hw), (reg)) + (index))

#define E1000_WRITE_REG_ARRAY_WORD(hw, reg, index, value) \
  e1000_writew((value), hw2membase(hw) + E1000_REGISTER((hw), (reg)) + ((index) << 1))

#define E1000_WRITE_REG_IO(hw, reg, value) outpd(hw2iobase(hw) + (reg), (value));

#define E1000_READ_FLASH_REG(hw, reg) e1000_readl(hw2flashbase(hw) + (reg))

#define E1000_READ_FLASH_REG16(hw, reg) e1000_readw(hw2flashbase(hw) + (reg))

#define E1000_WRITE_FLASH_REG(hw, reg, value) e1000_writel((value), hw2flashbase(hw) + (reg))

#define E1000_WRITE_FLASH_REG16(hw, reg, value) e1000_writew((value), hw2flashbase(hw) + (reg))

#define ASSERT_NO_LOCKS()

#endif /* _FUCHSIA_OS_H_ */
