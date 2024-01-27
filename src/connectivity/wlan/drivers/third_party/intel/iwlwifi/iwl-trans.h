/******************************************************************************
 *
 * Copyright(c) 2005 - 2014 Intel Corporation. All rights reserved.
 * Copyright(c) 2013 - 2015 Intel Mobile Communications GmbH
 * Copyright(c) 2016 - 2017 Intel Deutschland GmbH
 * Copyright(c) 2018        Intel Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *  * Neither the name Intel Corporation nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *****************************************************************************/
#ifndef SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_INTEL_IWLWIFI_IWL_TRANS_H_
#define SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_INTEL_IWLWIFI_IWL_TRANS_H_

#include <threads.h>
#include <lib/sync/completion.h>

#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/fw/img.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/iwl-config.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/iwl-debug.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/iwl-op-mode.h"
#include "zircon/compiler.h"
#ifdef CPTCFG_IWLWIFI_SUPPORT_DEBUG_OVERRIDES
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/iwl-dbg-cfg.h"
#endif
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/fw/api/cmdhdr.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/fw/api/txq.h"
#ifdef CPTCFG_IWLWIFI_DEVICE_TESTMODE
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/fw/testmode.h"
#endif
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/fw/api/dbg-tlv.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/iwl-dbg-tlv.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/platform/align.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/platform/ieee80211.h"
#include "src/connectivity/wlan/drivers/third_party/intel/iwlwifi/platform/memory.h"

__BEGIN_CDECLS

/**
 * DOC: Transport layer - what is it ?
 *
 * The transport layer is the layer that deals with the HW directly. It provides
 * an abstraction of the underlying HW to the upper layer. The transport layer
 * doesn't provide any policy, algorithm or anything of this kind, but only
 * mechanisms to make the HW do something. It is not completely stateless but
 * close to it.
 * We will have an implementation for each different supported bus.
 */

/**
 * DOC: Life cycle of the transport layer
 *
 * The transport layer has a very precise life cycle.
 *
 *  1) A helper function is called during the module initialization and
 *     registers the bus driver's ops with the transport's alloc function.
 *  2) Bus's probe calls to the transport layer's allocation functions.
 *     Of course this function is bus specific.
 *  3) This allocation functions will spawn the upper layer which will
 *     register mac80211.
 *
 *  4) At some point (i.e. mac80211's start call), the op_mode will call
 *     the following sequence:
 *     start_hw
 *     start_fw
 *
 *  5) Then when finished (or reset):
 *     stop_device
 *
 *  6) Eventually, the free function will be called.
 */

#define FH_RSCSR_FRAME_SIZE_MSK 0x00003FFF /* bits 0-13 */
#define FH_RSCSR_FRAME_INVALID 0x55550000
#define FH_RSCSR_FRAME_ALIGN 0x40
#define FH_RSCSR_RPA_EN BIT(25)
#define FH_RSCSR_RADA_EN BIT(26)
#define FH_RSCSR_RXQ_POS 16
#define FH_RSCSR_RXQ_MASK 0x3F0000

struct iwl_rx_packet {
  /*
   * The first 4 bytes of the RX frame header contain both the RX frame
   * size and some flags.
   * Bit fields:
   * 31:    flag flush RB request
   * 30:    flag ignore TC (terminal counter) request
   * 29:    flag fast IRQ request
   * 28-27: Reserved
   * 26:    RADA enabled
   * 25:    Offload enabled
   * 24:    RPF enabled
   * 23:    RSS enabled
   * 22:    Checksum enabled
   * 21-16: RX queue
   * 15-14: Reserved
   * 13-00: RX frame size (not including the first 4 bytes)
   */
  __le32 len_n_flags;
  struct iwl_cmd_header hdr;
  uint8_t data[0];
} __packed;

// The length including pkt->hdr and pkt->data[] (but not including len_n_flags).
static inline uint32_t iwl_rx_packet_len(const struct iwl_rx_packet* pkt) {
  return le32_to_cpu(pkt->len_n_flags) & FH_RSCSR_FRAME_SIZE_MSK;
}

// The length of pkt->data[].
static inline uint32_t iwl_rx_packet_payload_len(const struct iwl_rx_packet* pkt) {
  return iwl_rx_packet_len(pkt) - sizeof(pkt->hdr);
}

/**
 * enum CMD_MODE - how to send the host commands ?
 *
 * @CMD_ASYNC: Return right away and don't wait for the response
 * @CMD_WANT_SKB: Not valid with CMD_ASYNC. The caller needs the buffer of
 *  the response. The caller needs to call iwl_free_resp when done.
 * @CMD_HIGH_PRIO: The command is high priority - it goes to the front of the
 *  command queue, but after other high priority commands. Valid only
 *  with CMD_ASYNC.
 * @CMD_SEND_IN_IDLE: The command should be sent even when the trans is idle.
 * @CMD_MAKE_TRANS_IDLE: The command response should mark the trans as idle.
 * @CMD_WAKE_UP_TRANS: The command response should wake up the trans
 *  (i.e. mark it as non-idle).
 * @CMD_WANT_ASYNC_CALLBACK: the op_mode's async callback function must be
 *  called after this command completes. Valid only with CMD_ASYNC.
 * @CMD_SEND_IN_D3: Allow the command to be sent in D3 mode, relevant to
 *	SUSPEND and RESUME commands. We are in D3 mode when we set
 *	trans->system_pm_mode to IWL_PLAT_PM_MODE_D3.
 */
enum CMD_MODE {
  CMD_ASYNC = BIT(0),
  CMD_WANT_SKB = BIT(1),
  CMD_SEND_IN_RFKILL = BIT(2),
  CMD_SEND_IN_D3 = BIT(4),
  CMD_HIGH_PRIO = BIT(3),
  CMD_SEND_IN_IDLE = BIT(4),
  CMD_MAKE_TRANS_IDLE = BIT(5),
  CMD_WAKE_UP_TRANS = BIT(6),
  CMD_WANT_ASYNC_CALLBACK = BIT(7),
};

#define DEF_CMD_PAYLOAD_SIZE 320

// This value is returned when iwl_trans_read_mem32() is called but the hardware is busy.
#define HW_IS_BUSY 0xa5a5a5a5

/**
 * struct iwl_device_cmd
 *
 * For allocation of the command and tx queues, this establishes the overall
 * size of the largest command we send to uCode, except for commands that
 * aren't fully copied and use other TFD space.
 */
struct iwl_device_cmd {
  union {
    struct {
      struct iwl_cmd_header hdr; /* uCode API */
      uint8_t payload[DEF_CMD_PAYLOAD_SIZE];
    };
    struct {
      struct iwl_cmd_header_wide hdr_wide;
      uint8_t payload_wide[DEF_CMD_PAYLOAD_SIZE - sizeof(struct iwl_cmd_header_wide) +
                           sizeof(struct iwl_cmd_header)];
    };
  };
} __packed;

/**
 * struct iwl_device_tx_cmd - buffer for TX command
 * @hdr: the header
 * @payload: the payload placeholder
 *
 * The actual structure is sized dynamically according to need.
 */
struct iwl_device_tx_cmd {
	struct iwl_cmd_header hdr;
	u8 payload[];
} __packed;

#define TFD_MAX_PAYLOAD_SIZE (sizeof(struct iwl_device_cmd))

/*
 * number of transfer buffers (fragments) per transmit frame descriptor;
 * this is just the driver's idea, the hardware supports 20
 */
#define IWL_MAX_CMD_TBS_PER_TFD 2

/* We need 2 entries for the TX command and header, and another one might
 * be needed for potential data in the SKB's head. The remaining ones can
 * be used for frags.
 */
#define IWL_TRANS_MAX_FRAGS(trans) ((trans)->txqs.tfd.max_tbs - 3)

/**
 * enum iwl_hcmd_dataflag - flag for each one of the chunks of the command
 *
 * @IWL_HCMD_DFL_NOCOPY: By default, the command is copied to the host command's
 *  ring. The transport layer doesn't map the command's buffer to DMA, but
 *  rather copies it to a previously allocated DMA buffer. This flag tells
 *  the transport layer not to copy the command, but to map the existing
 *  buffer (that is passed in) instead. This saves the memcpy and allows
 *  commands that are bigger than the fixed buffer to be submitted.
 *  Note that a TFD entry after a NOCOPY one cannot be a normal copied one.
 * @IWL_HCMD_DFL_DUP: Only valid without NOCOPY, duplicate the memory for this
 *  chunk internally and free it again after the command completes. This
 *  can (currently) be used only once per command.
 *  Note that a TFD entry after a DUP one cannot be a normal copied one.
 */
enum iwl_hcmd_dataflag {
  IWL_HCMD_DFL_NOCOPY = BIT(0),
  IWL_HCMD_DFL_DUP = BIT(1),
};

/**
 * struct iwl_host_cmd - Host command to the uCode
 *
 * @data: array of chunks that composes the data of the host command
 * @resp_pkt: response packet, if %CMD_WANT_SKB was set.
 *            This variable is used by a Tx command (if the CMD_WANT_SKB bit is requested) and
 *            is assigned (to meta->source->resp_pkt) when the response is received from the
 *            firmware. See pcie/tx.c:iwl_pcie_hcmd_complete() for more details.
 * @flags: can be CMD_*
 * @len: array of the lengths of the chunks in data
 * @dataflags: IWL_HCMD_DFL_*
 * @id: command id of the host command, for wide commands encoding the
 *  version and group as well
 */
struct iwl_host_cmd {
  const void* data[IWL_MAX_CMD_TBS_PER_TFD];
  struct iwl_rx_packet* resp_pkt;

  uint32_t flags;
  uint32_t id;
  uint16_t len[IWL_MAX_CMD_TBS_PER_TFD];
  uint8_t dataflags[IWL_MAX_CMD_TBS_PER_TFD];
};

// Originally used by Linux to release the page mapping (says _rx_page_addr). But we don't need this
// in Fuchsia because the mapping info is maintained in io_buf.
//
// However, we keep the function (even it is empty) because calling function has semantic meaning in
// the code, which means the code will no longer accesses the resources after calling this function.
//
static inline void iwl_free_resp(struct iwl_host_cmd* cmd) {}

struct iwl_rx_cmd_buffer {
  struct iwl_iobuf* _iobuf;
  int _offset;
  uint8_t status;
};

static inline void* rxb_addr(struct iwl_rx_cmd_buffer* r) {
  return (char*)iwl_iobuf_virtual(r->_iobuf) + r->_offset;
}

static inline int rxb_offset(struct iwl_rx_cmd_buffer* r) { return r->_offset; }

static inline struct iwl_iobuf* rxb_steal_iobuf(struct iwl_rx_cmd_buffer* r) {
  // The Linux driver passes buffers up the stack by increasing the refcount on and returning the
  // page using get_page().  For Fuchsia, we take the simpler approach of copying data from the VMO
  // directly, and no refcount increment is needed.
  return r->_iobuf;
}

// Originally used by Linux to release the page mapping (says _rx_page_addr). But we don't need this
// in Fuchsia because the mapping info is maintained in io_buf.
//
// However, we keep the function (even it is empty) because calling function has semantic meaning in
// the code, which means the code will no longer accesses the resources after calling this function.
//
static inline void iwl_free_rxb(struct iwl_rx_cmd_buffer* r) {}

#define MAX_NO_RECLAIM_CMDS 6

#define IWL_MASK(lo, hi) ((1 << (hi)) | ((1 << (hi)) - (1 << (lo))))

/*
 * Maximum number of HW queues the transport layer
 * currently supports
 */
#define IWL_MAX_HW_QUEUES 32
#define IWL_MAX_TVQM_QUEUES 512

#define IWL_MAX_TID_COUNT 8
#define IWL_MGMT_TID 15
#define IWL_FRAME_LIMIT 64
#define IWL_MAX_RX_HW_QUEUES 16

/**
 * enum iwl_wowlan_status - WoWLAN image/device status
 * @IWL_D3_STATUS_ALIVE: firmware is still running after resume
 * @IWL_D3_STATUS_RESET: device was reset while suspended
 */
enum iwl_d3_status {
  IWL_D3_STATUS_ALIVE,
  IWL_D3_STATUS_RESET,
};

/**
 * enum iwl_trans_status: transport status flags
 * @STATUS_SYNC_HCMD_ACTIVE: a SYNC command is being processed
 * @STATUS_DEVICE_ENABLED: APM is enabled
 * @STATUS_TPOWER_PMI: the device might be asleep (need to wake it up)
 * @STATUS_INT_ENABLED: interrupts are enabled
 * @STATUS_RFKILL_HW: the actual HW state of the RF-kill switch
 * @STATUS_RFKILL_OPMODE: RF-kill state reported to opmode
 * @STATUS_FW_ERROR: the fw is in error state
 * @STATUS_TRANS_GOING_IDLE: shutting down the trans, only special commands
 *  are sent
 * @STATUS_TRANS_IDLE: the trans is idle - general commands are not to be sent
 * @STATUS_TA_ACTIVE: target access is in progress
 * @STATUS_TRANS_DEAD: trans is dead - avoid any read/write operation
 */
enum iwl_trans_status {
  STATUS_SYNC_HCMD_ACTIVE,
  STATUS_DEVICE_ENABLED,
  STATUS_TPOWER_PMI,
  STATUS_INT_ENABLED,
  STATUS_RFKILL_HW,
  STATUS_RFKILL_OPMODE,
  STATUS_FW_ERROR,
  STATUS_TRANS_GOING_IDLE,
  STATUS_TRANS_IDLE,
  STATUS_TRANS_DEAD,
  STATUS_SUPPRESS_CMD_ERROR_ONCE,
};

static inline size_t iwl_trans_get_rb_size_order(enum iwl_amsdu_size rb_size) {
  // Returns the equivalent of get_order(SIZE).
  switch (rb_size) {
    case IWL_AMSDU_2K:
      return 0;
    case IWL_AMSDU_4K:
      return 0;
    case IWL_AMSDU_8K:
      return 1;
    case IWL_AMSDU_12K:
      return 2;
    default:
      WARN_ON(1);
      return 0;
  }
}

static inline size_t
iwl_trans_get_rb_size(enum iwl_amsdu_size rb_size)
{
	switch (rb_size) {
	case IWL_AMSDU_2K:
		return 2 * 1024;
	case IWL_AMSDU_4K:
		return 4 * 1024;
	case IWL_AMSDU_8K:
		return 8 * 1024;
	case IWL_AMSDU_12K:
		return 16 * 1024;
	default:
		WARN_ON(1);
		return 0;
	}
}

struct iwl_hcmd_names {
  uint8_t cmd_id;
  const char* const cmd_name;
};

#define HCMD_NAME(x) \
  { .cmd_id = x, .cmd_name = #x }

struct iwl_hcmd_arr {
  const struct iwl_hcmd_names* arr;
  int size;
};

#define HCMD_ARR(x) \
  { .arr = x, .size = ARRAY_SIZE(x) }

/**
 * struct iwl_trans_config - transport configuration
 *
 * @op_mode: pointer to the upper layer.
 * @cmd_queue: the index of the command queue.
 *  Must be set before start_fw.
 * @cmd_fifo: the fifo for host commands
 * @cmd_q_wdg_timeout: the timeout of the watchdog timer for the command queue.
 * @no_reclaim_cmds: Some devices erroneously don't set the
 *  SEQ_RX_FRAME bit on some notifications, this is the
 *  list of such notifications to filter. Max length is
 *  %MAX_NO_RECLAIM_CMDS.
 * @n_no_reclaim_cmds: # of commands in list
 * @rx_buf_size: RX buffer size needed for A-MSDUs
 *  if unset 4k will be the RX buffer size
 * @bc_table_dword: set to true if the BC table expects the byte count to be
 *  in DWORD (as opposed to bytes)
 * @scd_set_active: should the transport configure the SCD for HCMD queue
 * @sw_csum_tx: transport should compute the TCP checksum
 * @command_groups: array of command groups, each member is an array of the
 *  commands in the group; for debugging only
 * @command_groups_size: number of command groups, to avoid illegal access
 * @cb_data_offs: offset inside skb->cb to store transport data at, must have
 *  space for at least two pointers
 */
struct iwl_trans_config {
  struct iwl_op_mode* op_mode;

  uint8_t cmd_queue;
  uint8_t cmd_fifo;
  unsigned int cmd_q_wdg_timeout;
  const uint8_t* no_reclaim_cmds;
  unsigned int n_no_reclaim_cmds;

  enum iwl_amsdu_size rx_buf_size;
  bool bc_table_dword;
  bool scd_set_active;
  bool sw_csum_tx;
  const struct iwl_hcmd_arr* command_groups;
  int command_groups_size;

  uint8_t cb_data_offs;
};

struct iwl_trans_dump_data {
  uint32_t len;
  uint8_t data[];
};

struct iwl_trans;

struct iwl_trans_txq_scd_cfg {
  uint8_t fifo;
  uint8_t sta_id;
  uint8_t tid;
  bool aggregate;
  int frame_limit;
};

/**
 * struct iwl_trans_rxq_dma_data - RX queue DMA data
 * @fr_bd_cb: DMA address of free BD cyclic buffer
 * @fr_bd_wid: Initial write index of the free BD cyclic buffer
 * @urbd_stts_wrptr: DMA address of urbd_stts_wrptr
 * @ur_bd_cb: DMA address of used BD cyclic buffer
 */
struct iwl_trans_rxq_dma_data {
  uint64_t fr_bd_cb;
  uint32_t fr_bd_wid;
  uint64_t urbd_stts_wrptr;
  uint64_t ur_bd_cb;
};

/**
 * struct iwl_trans_ops - transport specific operations
 *
 * All the handlers MUST be implemented
 *
 * @start_hw: starts the HW. If low_power is true, the NIC needs to be taken
 *  out of a low power state. From that point on, the HW can send
 *  interrupts. May sleep.
 * @op_mode_leave: Turn off the HW RF kill indication if on
 *  May sleep
 * @start_fw: allocates and inits all the resources for the transport
 *  layer. Also kick a fw image.
 *  May sleep
 * @fw_alive: called when the fw sends alive notification. If the fw provides
 *  the SCD base address in SRAM, then provide it here, or 0 otherwise.
 *  May sleep
 * @stop_device: stops the whole device (embedded CPU put to reset) and stops
 *  the HW. If low_power is true, the NIC will be put in low power state.
 *  From that point on, the HW will be stopped but will still issue an
 *  interrupt if the HW RF kill switch is triggered.
 *  This callback must do the right thing and not crash even if %start_hw()
 *  was called but not &start_fw(). May sleep.
 * @d3_suspend: put the device into the correct mode for WoWLAN during
 *  suspend. This is optional, if not implemented WoWLAN will not be
 *  supported. This callback may sleep.
 * @d3_resume: resume the device after WoWLAN, enabling the opmode to
 *  talk to the WoWLAN image to get its status. This is optional, if not
 *  implemented WoWLAN will not be supported. This callback may sleep.
 * @send_cmd:send a host command. Must return -ERFKILL if RFkill is asserted.
 *  If RFkill is asserted in the middle of a SYNC host command, it must
 *  return -ERFKILL straight away.
 *  May sleep only if CMD_ASYNC is not set
 * @tx: send an skb. The transport relies on the op_mode to zero the
 *  the ieee80211_tx_info->driver_data. If the MPDU is an A-MSDU, all
 *  the CSUM will be taken care of (TCP CSUM and IP header in case of
 *  IPv4). If the MPDU is a single MSDU, the op_mode must compute the IP
 *  header if it is IPv4.
 *  Must be atomic
 * @reclaim: free packet until ssn. Returns a list of freed packets.
 *  Must be atomic
 * @txq_enable: setup a queue. To setup an AC queue, use the
 *  iwl_trans_ac_txq_enable wrapper. fw_alive must have been called before
 *  this one. The op_mode must not configure the HCMD queue. The scheduler
 *  configuration may be %NULL, in which case the hardware will not be
 *  configured. If true is returned, the operation mode needs to increment
 *  the sequence number of the packets routed to this queue because of a
 *  hardware scheduler bug. May sleep.
 * @txq_disable: de-configure a Tx queue to send AMPDUs
 *  Must be atomic
 * @txq_set_shared_mode: change Tx queue shared/unshared marking
 * @wait_tx_queues_empty: wait until tx queues are empty. May sleep.
 * @wait_txq_empty: wait until specific tx queue is empty. May sleep.
 * @freeze_txq_timer: prevents the timer of the queue from firing until the
 *  queue is set to awake. Must be atomic.
 * @block_txq_ptrs: stop updating the write pointers of the Tx queues. Note
 *  that the transport needs to refcount the calls since this function
 *  will be called several times with block = true, and then the queues
 *  need to be unblocked only after the same number of calls with
 *  block = false.
 * @write8: write a uint8_t to a register at offset ofs from the BAR
 * @write32: write a uint32_t to a register at offset ofs from the BAR
 * @read32: read a uint32_t register at offset ofs from the BAR
 * @read_prph: read a DWORD from a periphery register
 * @write_prph: write a DWORD to a periphery register
 * @read_mem: read device's SRAM in DWORD
 * @write_mem: write device's SRAM in DWORD. If %buf is %NULL, then the memory
 *  will be zeroed.
 * @configure: configure parameters required by the transport layer from
 *  the op_mode. May be called several times before start_fw, can't be
 *  called after that.
 * @set_pmi: set the power pmi state
 * @grab_nic_access: wake the NIC to be able to access non-HBUS regs.
 *  Sleeping is not allowed between grab_nic_access and
 *  release_nic_access.
 * @release_nic_access: let the NIC go to sleep. The "flags" parameter
 *  must be the same one that was sent before to the grab_nic_access.
 * @set_bits_mask - set SRAM register according to value and mask.
 * @ref: grab a reference to the transport/FW layers, disallowing
 *  certain low power states
 * @unref: release a reference previously taken with @ref. Note that
 *  initially the reference count is 1, making an initial @unref
 *  necessary to allow low power states.
 * @dump_data: return a vmalloc'ed buffer with debug data, maybe containing last
 *  TX'ed commands and similar. The buffer will be vfree'd by the caller.
 *  Note that the transport must fill in the proper file headers.
 * @debugfs_cleanup: used in the driver unload flow to make a proper cleanup
 *  of the trans debugfs
 */
struct iwl_trans_ops {
  zx_status_t (*start_hw)(struct iwl_trans* iwl_trans, bool low_power);
  void (*op_mode_leave)(struct iwl_trans* iwl_trans);
#if IS_ENABLED(CPTCFG_IWLXVT)
  int (*start_fw_dbg)(struct iwl_trans* trans, const struct fw_img* fw, bool run_in_rfkill,
                      uint32_t fw_dbg_flags);
  int (*test_mode_cmd)(struct iwl_trans* trans, bool enable);
#endif
  zx_status_t (*start_fw)(struct iwl_trans* trans, const struct fw_img* fw, bool run_in_rfkill);
  void (*fw_alive)(struct iwl_trans* trans, uint32_t scd_addr);
  void (*stop_device)(struct iwl_trans* trans);

  void (*d3_suspend)(struct iwl_trans* trans, bool test, bool reset);
  zx_status_t (*d3_resume)(struct iwl_trans* trans, enum iwl_d3_status* status, bool test,
                           bool reset);

  zx_status_t (*send_cmd)(struct iwl_trans* trans, struct iwl_host_cmd* cmd);

  zx_status_t (*tx)(struct iwl_trans* trans, struct ieee80211_mac_packet* pkt,
                    struct iwl_device_tx_cmd* dev_cmd, int queue);
  void (*reclaim)(struct iwl_trans* trans, int queue, int ssn);

  bool (*txq_enable)(struct iwl_trans* trans, int queue, uint16_t ssn,
                     const struct iwl_trans_txq_scd_cfg* cfg, zx_duration_t queue_wdg_timeout);
  void (*txq_disable)(struct iwl_trans* trans, int queue, bool configure_scd);
  /* 22000 functions */
  zx_status_t (*txq_alloc)(struct iwl_trans* trans, __le16 flags, uint8_t sta_id, uint8_t tid,
                           int cmd_id, int size, unsigned int queue_wdg_timeout);
  void (*txq_free)(struct iwl_trans* trans, int queue);
  zx_status_t (*rxq_dma_data)(struct iwl_trans* trans, int queue,
                              struct iwl_trans_rxq_dma_data* data);

  void (*txq_set_shared_mode)(struct iwl_trans* trans, uint32_t txq_id, bool shared);

  zx_status_t (*wait_tx_queues_empty)(struct iwl_trans* trans, uint32_t txq_bm);
  zx_status_t (*wait_txq_empty)(struct iwl_trans* trans, int queue);
  void (*freeze_txq_timer)(struct iwl_trans* trans, unsigned long txqs, bool freeze);
  void (*block_txq_ptrs)(struct iwl_trans* trans, bool block);

  void (*write8)(struct iwl_trans* trans, uint32_t ofs, uint8_t val);
  void (*write32)(struct iwl_trans* trans, uint32_t ofs, uint32_t val);
  uint32_t (*read32)(struct iwl_trans* trans, uint32_t ofs);
  uint32_t (*read_prph)(struct iwl_trans* trans, uint32_t ofs);
  void (*write_prph)(struct iwl_trans* trans, uint32_t ofs, uint32_t val);
  zx_status_t (*read_mem)(struct iwl_trans* trans, uint32_t addr, void* buf, size_t dwords);
  zx_status_t (*write_mem)(struct iwl_trans* trans, uint32_t addr, const void* buf, size_t dwords);
  void (*configure)(struct iwl_trans* trans, const struct iwl_trans_config* trans_cfg);
  void (*set_pmi)(struct iwl_trans* trans, bool state);
  zx_status_t (*sw_reset)(struct iwl_trans* trans, bool retake_ownership);
  bool (*grab_nic_access)(struct iwl_trans* trans);
  void (*release_nic_access)(struct iwl_trans* trans, unsigned long* flags);
  void (*set_bits_mask)(struct iwl_trans* trans, uint32_t reg, uint32_t mask, uint32_t value);
  void (*ref)(struct iwl_trans* trans);
  void (*unref)(struct iwl_trans* trans);
  zx_status_t (*suspend)(struct iwl_trans* trans);
  void (*resume)(struct iwl_trans* trans);

  struct iwl_trans_dump_data* (*dump_data)(struct iwl_trans* trans, uint32_t dump_mask);
  void (*debugfs_cleanup)(struct iwl_trans* trans);
	void (*sync_nmi)(struct iwl_trans *trans);
	int (*set_pnvm)(struct iwl_trans *trans, const void *data, u32 len);
	int (*set_reduce_power)(struct iwl_trans *trans,
				const void *data, u32 len);
	void (*interrupts)(struct iwl_trans *trans, bool enable);
	int (*imr_dma_data)(struct iwl_trans *trans,
			    u32 dst_addr, u64 src_addr,
			    u32 byte_cnt);

};

/**
 * enum iwl_trans_state - state of the transport layer
 *
 * @IWL_TRANS_NO_FW: firmware wasn't started yet, or crashed
 * @IWL_TRANS_FW_STARTED: FW was started, but not alive yet
 * @IWL_TRANS_FW_ALIVE: FW has sent an alive response
 */
enum iwl_trans_state {
	IWL_TRANS_NO_FW,
	IWL_TRANS_FW_STARTED,
	IWL_TRANS_FW_ALIVE,
};

/**
 * DOC: Platform power management
 *
 * There are two types of platform power management: system-wide
 * (WoWLAN) and runtime.
 *
 * In system-wide power management the entire platform goes into a low
 * power state (e.g. idle or suspend to RAM) at the same time and the
 * device is configured as a wakeup source for the entire platform.
 * This is usually triggered by userspace activity (e.g. the user
 * presses the suspend button or a power management daemon decides to
 * put the platform in low power mode).  The device's behavior in this
 * mode is dictated by the wake-on-WLAN configuration.
 *
 * In runtime power management, only the devices which are themselves
 * idle enter a low power state.  This is done at runtime, which means
 * that the entire system is still running normally.  This mode is
 * usually triggered automatically by the device driver and requires
 * the ability to enter and exit the low power modes in a very short
 * time, so there is not much impact in usability.
 *
 * The terms used for the device's behavior are as follows:
 *
 *  - D0: the device is fully powered and the host is awake;
 *  - D3: the device is in low power mode and only reacts to
 *      specific events (e.g. magic-packet received or scan
 *      results found);
 *  - D0I3: the device is in low power mode and reacts to any
 *      activity (e.g. RX);
 *
 * These terms reflect the power modes in the firmware and are not to
 * be confused with the physical device power state.  The NIC can be
 * in D0I3 mode even if, for instance, the PCI device is in D3 state.
 */

/**
 * enum iwl_plat_pm_mode - platform power management mode
 *
 * This enumeration describes the device's platform power management
 * behavior when in idle mode (i.e. runtime power management) or when
 * in system-wide suspend (i.e WoWLAN).
 *
 * @IWL_PLAT_PM_MODE_DISABLED: power management is disabled for this
 *  device.  At runtime, this means that nothing happens and the
 *  device always remains in active.  In system-wide suspend mode,
 *  it means that the all connections will be closed automatically
 *  by mac80211 before the platform is suspended.
 * @IWL_PLAT_PM_MODE_D3: the device goes into D3 mode (i.e. WoWLAN).
 *  For runtime power management, this mode is not officially
 *  supported.
 * @IWL_PLAT_PM_MODE_D0I3: the device goes into D0I3 mode.
 */
enum iwl_plat_pm_mode {
  IWL_PLAT_PM_MODE_DISABLED,
  IWL_PLAT_PM_MODE_D3,
  IWL_PLAT_PM_MODE_D0I3,
};

/* Max time to wait for trans to become idle/non-idle on d0i3
 * enter/exit (in msecs).
 */
#define IWL_TRANS_IDLE_TIMEOUT (CPTCFG_IWL_TIMEOUT_FACTOR * 2000)

/**
 * struct iwl_dram_data
 * @physical: page phy pointer
 * @block: pointer to the allocated block/page
 * @size: size of the block/page
 */
struct iwl_dram_data {
  dma_addr_t physical;
  void* block;
  int size;
};

struct iwl_dma_ptr {
	struct iwl_iobuf* io_buf;
	dma_addr_t dma;
	void *addr;
	size_t size;
};

struct iwl_cmd_meta {
	/* only for SYNC commands, iff the reply skb is wanted */
	struct iwl_host_cmd *source;
	u32 flags;
	u32 tbs;
};

/*
 * The FH will write back to the first TB only, so we need to copy some data
 * into the buffer regardless of whether it should be mapped or not.
 * This indicates how big the first TB must be to include the scratch buffer
 * and the assigned PN.
 * Since PN location is 8 bytes at offset 12, it's 20 now.
 * If we make it bigger then allocations will be bigger and copy slower, so
 * that's probably not useful.
 */
#define IWL_FIRST_TB_SIZE	20
#define IWL_FIRST_TB_SIZE_ALIGN ALIGN(IWL_FIRST_TB_SIZE, 64)

struct iwl_pcie_txq_entry {
  struct iwl_iobuf* cmd;  // Used to store the command
  struct iwl_iobuf* dup_io_buf;
  struct iwl_cmd_meta meta;
};

struct iwl_pcie_first_tb_buf {
	u8 buf[IWL_FIRST_TB_SIZE_ALIGN];
};

/**
 * struct iwl_txq - Tx Queue for DMA
 * @q: generic Rx/Tx queue descriptor
 * @tfds: transmit frame descriptors (DMA memory)
 * @first_tb_bufs: start of command headers, including scratch buffers, for
 *	the writeback -- this is DMA memory and an array holding one buffer
 *	for each command on the queue
 * @first_tb_dma: DMA address for the first_tb_bufs start
 * @entries: transmit entries (driver state)
 * @lock: queue lock
 * @stuck_timer: timer that fires if queue gets stuck
 * @trans: pointer back to transport (for timer)
 * @need_update: indicates need to update read/write index
 * @ampdu: true if this queue is an ampdu queue for an specific RA/TID
 * @wd_timeout: queue watchdog timeout (jiffies) - per queue
 * @frozen: tx stuck queue timer is frozen
 * @frozen_expiry_remainder: remember how long until the timer fires
 * @bc_tbl: byte count table of the queue (relevant only for gen2 transport)
 * @write_ptr: 1-st empty entry (index) host_w
 * @read_ptr: last used entry (index) host_r
 * @dma_addr:  physical addr for BD's
 * @n_window: safe queue window
 * @id: queue id
 * @low_mark: low watermark, resume queue if free space more than this
 * @high_mark: high watermark, stop queue if free space less than this
 *
 * A Tx queue consists of circular buffer of BDs (a.k.a. TFDs, transmit frame
 * descriptors) and required locking structures.
 *
 * Note the difference between TFD_QUEUE_SIZE_MAX and n_window: the hardware
 * always assumes 256 descriptors, so TFD_QUEUE_SIZE_MAX is always 256 (unless
 * there might be HW changes in the future). For the normal TX
 * queues, n_window, which is the size of the software queue data
 * is also 256; however, for the command queue, n_window is only
 * 32 since we don't need so many commands pending. Since the HW
 * still uses 256 BDs for DMA though, TFD_QUEUE_SIZE_MAX stays 256.
 * This means that we end up with the following:
 *  HW entries: | 0 | ... | N * 32 | ... | N * 32 + 31 | ... | 255 |
 *  SW entries:           | 0      | ... | 31          |
 * where N is a number between 0 and 7. This means that the SW
 * data is a window overlayed over the HW queue.
 */
struct iwl_txq {
	struct iwl_iobuf* tfds;
	struct iwl_iobuf* first_tb_bufs;
	dma_addr_t first_tb_dma;
	struct iwl_pcie_txq_entry *entries;
	/* lock for syncing changes on the queue */
	mtx_t lock;
	unsigned long frozen_expiry_remainder;
	struct iwl_irq_timer* stuck_timer;
	struct iwl_trans_pcie* trans_pcie;
	struct iwl_trans *trans;
	bool need_update;
	bool frozen;
	bool ampdu;
	int block;
	zx_duration_t wd_timeout;
	struct sk_buff_head overflow_q;
	struct iwl_dma_ptr bc_tbl;

	int write_ptr;
	int read_ptr;
	dma_addr_t dma_addr;
	int n_window;
	u32 id;
	int low_mark;
	int high_mark;

	bool overflow_tx;
};

/**
 * struct iwl_trans_txqs - transport tx queues data
 *
 * @bc_table_dword: true if the BC table expects DWORD (as opposed to bytes)
 * @page_offs: offset from skb->cb to mac header page pointer
 * @dev_cmd_offs: offset from skb->cb to iwl_device_tx_cmd pointer
 * @queue_used - bit mask of used queues
 * @queue_stopped - bit mask of stopped queues
 * @scd_bc_tbls: gen1 pointer to the byte count table of the scheduler
 * @queue_alloc_cmd_ver: queue allocation command version
 */
struct iwl_trans_txqs {
	unsigned long queue_used[BITS_TO_LONGS(IWL_MAX_TVQM_QUEUES)];
	unsigned long queue_stopped[BITS_TO_LONGS(IWL_MAX_TVQM_QUEUES)];
	struct iwl_txq *txq[IWL_MAX_TVQM_QUEUES];
	struct dma_pool *bc_pool;
	size_t bc_tbl_size;
	bool bc_table_dword;
	u8 page_offs;
	u8 dev_cmd_offs;

	struct {
		u8 fifo;
		u8 q_id;
		unsigned int wdg_timeout;
	} cmd;

	struct {
		u8 max_tbs;
		u16 size;
		u8 addr_size;
	} tfd;

	struct iwl_dma_ptr scd_bc_tbls;

	u8 queue_alloc_cmd_ver;
};

/**
 * struct iwl_trans - transport common data
 *
 * @ops - pointer to iwl_trans_ops
 * @op_mode - pointer to the op_mode
 * @trans_cfg: the trans-specific configuration part
 * @cfg - pointer to the configuration
 * @drv - pointer to iwl_drv
 * @status: a bit-mask of transport status flags
 * @dev - pointer to struct device * that represents the device
 * @max_skb_frags: maximum number of fragments an SKB can have when transmitted.
 *  0 indicates that frag SKBs (NETIF_F_SG) aren't supported.
 * @hw_rf_id a uint32_t with the device RF ID
 * @hw_id: a uint32_t with the ID of the device / sub-device.
 *  Set during transport allocation.
 * @hw_id_str: a string with info about HW ID. Set during transport allocation.
 * @hw_rev_step: The mac step of the HW
 * @pm_support: set to true in start_hw if link pm is supported
 * @ltr_enabled: set to true if the LTR is enabled
 * @wide_cmd_header: true when ucode supports wide command header format
 * @wait_command_queue: wait queue for sync commands
 * @num_rx_queues: number of RX queues allocated by the transport;
 *  the transport must set this before calling iwl_drv_start()
 * @iml_len: the length of the image loader
 * @iml: a pointer to the image loader itself
 * @dev_cmd_pool: pool for Tx cmd allocation - for internal use only.
 *  The user should use iwl_trans_{alloc,free}_tx_cmd.
 * @rx_mpdu_cmd: MPDU RX command ID, must be assigned by opmode before
 *  starting the firmware, used for tracing
 * @rx_mpdu_cmd_hdr_size: used for tracing, amount of data before the
 *  start of the 802.11 header in the @rx_mpdu_cmd
 * @dflt_pwr_limit: default power limit fetched from the platform (ACPI)
 * @dbg_dest_tlv: points to the destination TLV for debug
 * @dbg_conf_tlv: array of pointers to configuration TLVs for debug
 * @dbg_trigger_tlv: array of pointers to triggers TLVs for debug
 * @dbg_n_dest_reg: num of reg_ops in %dbg_dest_tlv
 * @num_blocks: number of blocks in fw_mon
 * @fw_mon: address of the buffers for firmware monitor
 * @system_pm_mode: the system-wide power management mode in use.
 *  This mode is set dynamically, depending on the WoWLAN values
 *  configured from the userspace at runtime.
 * @iwl_trans_txqs: transport tx queues data.
 */
struct iwl_trans {
  struct iwl_trans_ops* ops;  // removed 'const' for unit test.
  struct iwl_op_mode* op_mode;
  const struct iwl_cfg_trans_params *trans_cfg;
  const struct iwl_cfg* cfg;
  struct iwl_drv* drv;
  struct iwl_tm_gnl_dev* tmdev;
  enum iwl_trans_state state;
  unsigned long status;

  zx_device_t* zxdev;
  struct device* dev;
	u32 max_skb_frags;
	u32 hw_rev;
	u32 hw_rev_step;
	u32 hw_rf_id;
	u32 hw_id;
  char hw_id_str[52];

  uint8_t rx_mpdu_cmd, rx_mpdu_cmd_hdr_size;

  bool pm_support;
  bool ltr_enabled;

  const struct iwl_hcmd_arr* command_groups;
  int command_groups_size;
  bool wide_cmd_header;

  sync_completion_t wait_command_queue;
  uint8_t num_rx_queues;

  size_t iml_len;
  uint8_t* iml;

  /* The following fields are internal only */
  struct dentry* dbgfs_dir;

#ifdef CPTCFG_IWLWIFI_SUPPORT_DEBUG_OVERRIDES
  struct iwl_dbg_cfg dbg_cfg;
#endif
  struct iwl_apply_point_data apply_points[IWL_FW_INI_APPLY_NUM];
  struct iwl_apply_point_data apply_points_ext[IWL_FW_INI_APPLY_NUM];

  bool external_ini_loaded;
  bool ini_valid;

  const struct iwl_fw_dbg_dest_tlv_v1* dbg_dest_tlv;
  const struct iwl_fw_dbg_conf_tlv* dbg_conf_tlv[FW_DBG_CONF_MAX];
  struct iwl_fw_dbg_trigger_tlv* const* dbg_trigger_tlv;
  uint8_t dbg_n_dest_reg;
  int num_blocks;
  struct iwl_dram_data fw_mon[IWL_FW_INI_APPLY_NUM];

  enum iwl_plat_pm_mode system_pm_mode;

	const char *name;
	struct iwl_trans_txqs txqs;

  /* pointer to trans specific struct */
  /*Ensure that this pointer will always be aligned to sizeof pointer */
  char trans_specific[] __aligned(sizeof(void*));
};

const char* iwl_get_cmd_string(struct iwl_trans* trans, uint32_t id);
int iwl_cmd_groups_verify_sorted(const struct iwl_trans_config* trans);

static inline void iwl_trans_configure(struct iwl_trans* trans,
                                       const struct iwl_trans_config* trans_cfg) {
  trans->op_mode = trans_cfg->op_mode;

  trans->ops->configure(trans, trans_cfg);
  WARN_ON(iwl_cmd_groups_verify_sorted(trans_cfg));
}

static inline int _iwl_trans_start_hw(struct iwl_trans* trans, bool low_power) {
  return trans->ops->start_hw(trans, low_power);
}

static inline int iwl_trans_start_hw(struct iwl_trans* trans) {
  return trans->ops->start_hw(trans, true);
}

static inline void iwl_trans_op_mode_leave(struct iwl_trans* trans) {
  if (trans->ops->op_mode_leave) {
    trans->ops->op_mode_leave(trans);
  }

  trans->op_mode = NULL;

  trans->state = IWL_TRANS_NO_FW;
}

static inline void iwl_trans_fw_alive(struct iwl_trans* trans, uint32_t scd_addr) {
  trans->state = IWL_TRANS_FW_ALIVE;

  trans->ops->fw_alive(trans, scd_addr);
}

static inline int iwl_trans_start_fw(struct iwl_trans* trans, const struct fw_img* fw,
                                     bool run_in_rfkill) {
  WARN_ON_ONCE(!trans->rx_mpdu_cmd);

  clear_bit(STATUS_FW_ERROR, &trans->status);
  return trans->ops->start_fw(trans, fw, run_in_rfkill);
}

#if IS_ENABLED(CPTCFG_IWLXVT)
enum iwl_xvt_dbg_flags {
  IWL_XVT_DBG_ADC_SAMP_TEST = BIT(0),
  IWL_XVT_DBG_ADC_SAMP_SYNC_RX = BIT(1),
};

static inline int iwl_trans_start_fw_dbg(struct iwl_trans* trans, const struct fw_img* fw,
                                         bool run_in_rfkill, uint32_t dbg_flags) {
  if (WARN_ON_ONCE(!trans->ops->start_fw_dbg && dbg_flags)) {
    return -ENOTSUPP;
  }

  clear_bit(STATUS_FW_ERROR, &trans->status);
  if (trans->ops->start_fw_dbg) {
    return trans->ops->start_fw_dbg(trans, fw, run_in_rfkill, dbg_flags);
  }

  return trans->ops->start_fw(trans, fw, run_in_rfkill);
}
#endif

static inline void _iwl_trans_stop_device(struct iwl_trans* trans, bool low_power) {
  trans->ops->stop_device(trans);

  trans->state = IWL_TRANS_NO_FW;
}

static inline void iwl_trans_stop_device(struct iwl_trans* trans) {
  _iwl_trans_stop_device(trans, true);
}

static inline void iwl_trans_d3_suspend(struct iwl_trans* trans, bool test, bool reset) {
  if (trans->ops->d3_suspend) {
    trans->ops->d3_suspend(trans, test, reset);
  }
}

static inline int iwl_trans_d3_resume(struct iwl_trans* trans, enum iwl_d3_status* status,
                                      bool test, bool reset) {
  if (!trans->ops->d3_resume) {
    return 0;
  }

  return trans->ops->d3_resume(trans, status, test, reset);
}

static inline int iwl_trans_suspend(struct iwl_trans* trans) {
  if (!trans->ops->suspend) {
    return 0;
  }

  return trans->ops->suspend(trans);
}

static inline void iwl_trans_resume(struct iwl_trans* trans) {
  if (trans->ops->resume) {
    trans->ops->resume(trans);
  }
}

static inline struct iwl_trans_dump_data* iwl_trans_dump_data(struct iwl_trans* trans,
                                                              uint32_t dump_mask) {
  if (!trans->ops->dump_data) {
    return NULL;
  }
  return trans->ops->dump_data(trans, dump_mask);
}

static inline struct iwl_device_tx_cmd *
iwl_trans_alloc_tx_cmd(struct iwl_trans *trans)
{
	// Allocate a maximum-length packet (`struct iwl_device_cmd`), but return it in the
	// variable-length form of header (`struct iwl_device_tx_cmd`).
	return (struct iwl_device_tx_cmd *)calloc(1, sizeof(struct iwl_device_cmd));
}

// This function returns couple error codes. The ZX_ERR_BAD_STATE is the most special one.
// It is called ERFKILL originally. We remap it to ZX_ERR_BAD_STATE in Fuchsia.
zx_status_t iwl_trans_send_cmd(struct iwl_trans* trans, struct iwl_host_cmd* cmd);

static inline void iwl_trans_free_tx_cmd(struct iwl_trans* trans,
    struct iwl_device_tx_cmd* dev_cmd) {
  free(dev_cmd);
}

static inline zx_status_t iwl_trans_tx(struct iwl_trans* trans, struct ieee80211_mac_packet* pkt,
                                       struct iwl_device_tx_cmd* dev_cmd, int queue) {
  if (unlikely(test_bit(STATUS_FW_ERROR, &trans->status))) {
    IWL_ERR(trans, "%s() trans->status inidicates FW_ERROR\n", __func__);
    return ZX_ERR_INTERNAL;
  }

  if (WARN_ON_ONCE(trans->state != IWL_TRANS_FW_ALIVE)) {
    IWL_ERR(trans, "%s bad state = %d\n", __func__, trans->state);
    return ZX_ERR_BAD_STATE;
  }

  return trans->ops->tx(trans, pkt, dev_cmd, queue);
}

static inline void iwl_trans_reclaim(struct iwl_trans* trans, int queue, int ssn) {
  if (WARN_ON_ONCE(trans->state != IWL_TRANS_FW_ALIVE)) {
    IWL_ERR(trans, "%s bad state = %d\n", __func__, trans->state);
    return;
  }

  trans->ops->reclaim(trans, queue, ssn);
}

static inline void iwl_trans_txq_disable(struct iwl_trans* trans, int queue, bool configure_scd) {
  trans->ops->txq_disable(trans, queue, configure_scd);
}

static inline bool iwl_trans_txq_enable_cfg(struct iwl_trans* trans, int queue, uint16_t ssn,
                                            const struct iwl_trans_txq_scd_cfg* cfg,
                                            zx_duration_t queue_wdg_timeout) {
  if (WARN_ON_ONCE(trans->state != IWL_TRANS_FW_ALIVE)) {
    IWL_ERR(trans, "%s bad state = %d\n", __func__, trans->state);
    return false;
  }

  return trans->ops->txq_enable(trans, queue, ssn, cfg, queue_wdg_timeout);
}

#if 0   // NEEDS_PORTING
static inline int
iwl_trans_get_rxq_dma_data(struct iwl_trans* trans, int queue,
                           struct iwl_trans_rxq_dma_data* data) {
    if (WARN_ON_ONCE(!trans->ops->rxq_dma_data)) {
        return -ENOTSUPP;
    }

    return trans->ops->rxq_dma_data(trans, queue, data);
}
#endif  // NEEDS_PORTING

static inline void iwl_trans_txq_free(struct iwl_trans* trans, int queue) {
  if (WARN_ON_ONCE(!trans->ops->txq_free)) {
    return;
  }

  trans->ops->txq_free(trans, queue);
}

static inline zx_status_t iwl_trans_txq_alloc(struct iwl_trans* trans, __le16 flags, uint8_t sta_id,
                                              uint8_t tid, int cmd_id, int size,
                                              unsigned int wdg_timeout) {
  if (WARN_ON_ONCE(!trans->ops->txq_alloc)) {
    return ZX_ERR_NOT_SUPPORTED;
  }

  if (WARN_ON_ONCE(trans->state != IWL_TRANS_FW_ALIVE)) {
    IWL_ERR(trans, "%s bad state = %d\n", __func__, trans->state);
    return ZX_ERR_IO;
  }

  return trans->ops->txq_alloc(trans, flags, sta_id, tid, cmd_id, size, wdg_timeout);
}

#if 0   // NEEDS_PORTING
static inline void iwl_trans_txq_set_shared_mode(struct iwl_trans* trans,
        int queue, bool shared_mode) {
    if (trans->ops->txq_set_shared_mode) {
        trans->ops->txq_set_shared_mode(trans, queue, shared_mode);
    }
}

static inline void iwl_trans_txq_enable(struct iwl_trans* trans, int queue,
                                        int fifo, int sta_id, int tid,
                                        int frame_limit, uint16_t ssn,
                                        unsigned int queue_wdg_timeout) {
    struct iwl_trans_txq_scd_cfg cfg = {
        .fifo = fifo,
        .sta_id = sta_id,
        .tid = tid,
        .frame_limit = frame_limit,
        .aggregate = sta_id >= 0,
    };

    iwl_trans_txq_enable_cfg(trans, queue, ssn, &cfg, queue_wdg_timeout);
}
#endif  // NEEDS_PORTING

static inline void iwl_trans_ac_txq_enable(struct iwl_trans* trans, int queue, uint8_t fifo,
                                           zx_duration_t queue_wdg_timeout) {
  struct iwl_trans_txq_scd_cfg cfg = {
      .fifo = fifo,
      .sta_id = UINT8_MAX,
      .tid = IWL_MAX_TID_COUNT,
      .aggregate = false,
      .frame_limit = IWL_FRAME_LIMIT,
  };

  iwl_trans_txq_enable_cfg(trans, queue, 0, &cfg, queue_wdg_timeout);
}

#if 0   // NEEDS_PORTING
static inline void iwl_trans_freeze_txq_timer(struct iwl_trans* trans,
        unsigned long txqs,
        bool freeze) {
    if (WARN_ON_ONCE(trans->state != IWL_TRANS_FW_ALIVE)) {
        IWL_ERR(trans, "%s bad state = %d\n", __func__, trans->state);
        return;
    }

    if (trans->ops->freeze_txq_timer) {
        trans->ops->freeze_txq_timer(trans, txqs, freeze);
    }
}
#endif  // NEEDS_PORTING

static inline void iwl_trans_block_txq_ptrs(struct iwl_trans* trans, bool block) {
  if (WARN_ON_ONCE(trans->state != IWL_TRANS_FW_ALIVE)) {
    IWL_ERR(trans, "%s bad state = %d\n", __func__, trans->state);
    return;
  }

  if (trans->ops->block_txq_ptrs) {
    trans->ops->block_txq_ptrs(trans, block);
  }
}

static inline zx_status_t iwl_trans_wait_tx_queues_empty(struct iwl_trans* trans, uint32_t txqs) {
  if (WARN_ON_ONCE(!trans->ops->wait_tx_queues_empty)) {
    return ZX_ERR_NOT_SUPPORTED;
  }

  if (WARN_ON_ONCE(trans->state != IWL_TRANS_FW_ALIVE)) {
    IWL_ERR(trans, "%s bad state = %d\n", __func__, trans->state);
    return ZX_ERR_BAD_STATE;
  }

  return trans->ops->wait_tx_queues_empty(trans, txqs);
}

static inline zx_status_t iwl_trans_wait_txq_empty(struct iwl_trans* trans, int queue) {
  if (WARN_ON_ONCE(!trans->ops->wait_txq_empty)) {
    return ZX_ERR_NOT_SUPPORTED;
  }

  if (WARN_ON_ONCE(trans->state != IWL_TRANS_FW_ALIVE)) {
    IWL_ERR(trans, "%s bad state = %d\n", __func__, trans->state);
    return ZX_ERR_BAD_STATE;
  }

  return trans->ops->wait_txq_empty(trans, queue);
}

#if IS_ENABLED(CPTCFG_IWLXVT)
static inline int iwl_trans_test_mode_cmd(struct iwl_trans* trans, bool enable) {
  if (trans->ops->test_mode_cmd) {
    return trans->ops->test_mode_cmd(trans, enable);
  }
  return -ENOTSUPP;
}
#endif

static inline void iwl_trans_write8(struct iwl_trans* trans, uint32_t ofs, uint8_t val) {
  trans->ops->write8(trans, ofs, val);
}

static inline void iwl_trans_write32(struct iwl_trans* trans, uint32_t ofs, uint32_t val) {
  trans->ops->write32(trans, ofs, val);
}

static inline uint32_t iwl_trans_read32(struct iwl_trans* trans, uint32_t ofs) {
  return trans->ops->read32(trans, ofs);
}

static inline uint32_t iwl_trans_read_prph(struct iwl_trans* trans, uint32_t ofs) {
  return trans->ops->read_prph(trans, ofs);
}

static inline void iwl_trans_write_prph(struct iwl_trans* trans, uint32_t ofs, uint32_t val) {
  return trans->ops->write_prph(trans, ofs, val);
}

static inline zx_status_t iwl_trans_read_mem(struct iwl_trans* trans, uint32_t addr, void* buf,
                                             size_t dwords) {
  return trans->ops->read_mem(trans, addr, buf, dwords);
}

#define iwl_trans_read_mem_bytes(trans, addr, buf, bufsize)             \
  do {                                                                  \
    if (__builtin_constant_p(bufsize))                                  \
      BUILD_BUG_ON((bufsize) % sizeof(uint32_t));                       \
    iwl_trans_read_mem(trans, addr, buf, (bufsize) / sizeof(uint32_t)); \
  } while (0)

static inline uint32_t iwl_trans_read_mem32(struct iwl_trans* trans, uint32_t addr) {
  uint32_t value;

  if (WARN_ON(iwl_trans_read_mem(trans, addr, &value, 1))) {
    return HW_IS_BUSY;
  }

  return value;
}

static inline zx_status_t iwl_trans_write_mem(struct iwl_trans* trans, uint32_t addr,
                                              const void* buf, size_t dwords) {
  return trans->ops->write_mem(trans, addr, buf, dwords);
}

static inline uint32_t iwl_trans_write_mem32(struct iwl_trans* trans, uint32_t addr, uint32_t val) {
  return iwl_trans_write_mem(trans, addr, &val, 1);
}

static inline void iwl_trans_set_pmi(struct iwl_trans* trans, bool state) {
  if (trans->ops->set_pmi) {
    trans->ops->set_pmi(trans, state);
  }
}

static inline zx_status_t iwl_trans_sw_reset(struct iwl_trans* trans, bool retake_ownership) {
  if (trans->ops->sw_reset) {
    return trans->ops->sw_reset(trans, retake_ownership);
  }
  return ZX_OK;
}

static inline void iwl_trans_set_bits_mask(struct iwl_trans* trans, uint32_t reg, uint32_t mask,
                                           uint32_t value) {
  trans->ops->set_bits_mask(trans, reg, mask, value);
}

#define iwl_trans_grab_nic_access(trans, flags) ((trans)->ops->grab_nic_access(trans))

static inline void iwl_trans_release_nic_access(struct iwl_trans* trans, unsigned long* flags) {
  trans->ops->release_nic_access(trans, flags);
}

static inline void iwl_trans_fw_error(struct iwl_trans *trans, bool sync)
{
	if (WARN_ON_ONCE(!trans->op_mode))
		return;

	/* prevent double restarts due to the same erroneous FW */
	if (!test_and_set_bit(STATUS_FW_ERROR, &trans->status)) {
		iwl_op_mode_nic_error(trans->op_mode, sync);
		trans->state = IWL_TRANS_NO_FW;
	}
}

static inline bool iwl_trans_fw_running(struct iwl_trans *trans)
{
  return trans->state == IWL_TRANS_FW_ALIVE;
}

static inline void iwl_trans_sync_nmi(struct iwl_trans *trans)
{
	if (trans->ops->sync_nmi)
		trans->ops->sync_nmi(trans);
}

void iwl_trans_sync_nmi_with_addr(struct iwl_trans *trans, u32 inta_addr,
				  u32 sw_err_bit);

/*****************************************************
 * transport helper functions
 *****************************************************/
struct iwl_trans *iwl_trans_alloc(unsigned int priv_size,
			  struct device *dev,
			  struct iwl_trans_ops *ops,
			  const struct iwl_cfg_trans_params *cfg_trans);
int iwl_trans_init(struct iwl_trans *trans);
void iwl_trans_free(struct iwl_trans* trans);
void iwl_trans_ref(struct iwl_trans* trans);
void iwl_trans_unref(struct iwl_trans* trans);

/*****************************************************
 * driver (transport) register/unregister functions
 ******************************************************/
/* PCI */
int __must_check iwl_pci_register_driver(void);
void iwl_pci_unregister_driver(void);

__END_CDECLS

#endif  // SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_INTEL_IWLWIFI_IWL_TRANS_H_
