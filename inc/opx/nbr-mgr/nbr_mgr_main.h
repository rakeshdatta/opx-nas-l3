/*
 * Copyright (c) 2016 Dell Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License. You may obtain
 * a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
 *
 * THIS CODE IS PROVIDED ON AN  *AS IS* BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT
 * LIMITATION ANY IMPLIED WARRANTIES OR CONDITIONS OF TITLE, FITNESS
 * FOR A PARTICULAR PURPOSE, MERCHANTABLITY OR NON-INFRINGEMENT.
 *
 * See the Apache Version 2.0 License for specific language governing
 * permissions and limitations under the License.
 */

/*
 * filename: nbr_mgr_main.h
 *
 */

#ifndef _NBR_MGR_MAIN_H_
#define _NBR_MGR_MAIN_H_

#include "nbr_mgr_msgq.h"

/* ARP/ND status */
#define NBR_MGR_NUD_INCOMPLETE 0x01
#define NBR_MGR_NUD_REACHABLE  0x02
#define NBR_MGR_NUD_STALE      0x04
#define NBR_MGR_NUD_DELAY      0x08
#define NBR_MGR_NUD_PROBE      0x10
#define NBR_MGR_NUD_FAILED     0x20
#define NBR_MGR_NUD_NOARP      0x40
#define NBR_MGR_NUD_PERMANENT  0x80
#define NBR_MGR_NUD_NONE          0x00

#define NBR_MGR_BURST_RESOLVE_CNT 300
#define NBR_MGR_MAX_NBR_RETRY_CNT 10
#define NBR_MGR_MAX_NBR_REFRESH_MAC_LEARN_RETRY_CNT 100
#define NBR_MGR_BURST_RESOLVE_DELAY 1 /* Every 1 sec NBR_MGR_BURST_RESOLVE_CNT ARP
                                         messages will be sent from kernel */
#define NBR_MGR_DELAY_BURST_RESOLVE_DELAY 5 /* Every 5 sec NBR_MGR_BURST_RESOLVE_CNT ARP
                                         messages will be sent from kernel,
                                         this is useful to learn the MAC in the HW */

#define NBR_MGR_INTF_ADMIN_MSG 0x1
#define NBR_MGR_INTF_VLAN_MSG  0x2
#define NBR_MGR_DEFAULT_VRF_ID 0

enum {
    NBR_MGR_AUTO_REFRESH_INIT,
    NBR_MGR_AUTO_REFRESH_ENABLE,
    NBR_MGR_AUTO_REFRESH_DISABLE,
};

bool nbr_mgr_cps_init (void);
int nbr_mgr_netlink_main(void);
bool nbr_mgr_process_main(void);
int nbr_mgr_resolve_main(void);
int nbr_mgr_delay_resolve_main(void);
bool nbr_mgr_nbr_resolve(nbr_mgr_msg_type_t type, nbr_mgr_nbr_entry_t *p_nbr);
bool nbr_mgr_burst_resolve_handler(nbr_mgr_msg_t *p_msg);
bool nbr_mgr_program_npu(nbr_mgr_op_t op, const nbr_mgr_nbr_entry_t& entry);
bool nbr_mgr_notify_intf_status(nbr_mgr_op_t op, const nbr_mgr_intf_entry_t& entry);
bool nbr_mgr_get_all_nh(uint8_t af);

int nbr_mgr_enqueue_flush_msg(uint32_t if_index, hal_vrf_id_t vrf_id);
bool nbr_mgr_get_auto_refresh_status (const char *vrf_name, uint32_t family);
bool nbr_mgr_is_mac_present_in_hw(hal_mac_addr_t mac, hal_ifindex_t if_index,
                                  bool& is_mac_present_in_hw);

#endif
