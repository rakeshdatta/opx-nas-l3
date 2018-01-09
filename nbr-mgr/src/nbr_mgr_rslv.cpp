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
 * filename: nbr_mgr_nl_rslv.cpp
 */

#include <unistd.h>
#include "nbr_mgr_msgq.h"
#include "nbr_mgr_main.h"
#include "nbr_mgr_log.h"
#include "std_mac_utils.h"
#include "std_ip_utils.h"
#include "nbr_mgr_utils.h"

#include "cps_class_map.h"
#include "dell-base-routing.h"
#include "nas_os_l3.h"

int nbr_mgr_resolve_main(void)
{
    for (;;) {
        nbr_mgr_process_burst_resolve_msg(nbr_mgr_burst_resolve_handler);
    }
    return true;
}

int nbr_mgr_delay_resolve_main(void)
{
    for (;;) {
        nbr_mgr_process_delay_resolve_msg(nbr_mgr_burst_resolve_handler);
    }
    return true;
}

int nbr_mgr_enqueue_flush_msg(uint32_t if_index) {
    nbr_mgr_msg_t *p_msg = nullptr;

    NBR_MGR_LOG_INFO ("NAS_FLUSH","FLUSH msg to be enqueued for :%d", if_index);
    nbr_mgr_msg_uptr_t p_msg_uptr = nbr_mgr_alloc_unique_msg(&p_msg);
    if (p_msg == NULL) {
        NBR_MGR_LOG_ERR ("NAS_FLUSH","Memory alloc failed for NAS flush message");
        return false;
    }
    memset(p_msg, 0, sizeof(nbr_mgr_msg_t));
    p_msg->type = NBR_MGR_NAS_FLUSH_MSG;
    p_msg->flush.if_index = if_index;

    nbr_mgr_enqueue_netlink_nas_msg(std::move(p_msg_uptr));
    return true;
}

static cps_api_object_t nbr_mgr_nbr_to_cps_obj(nbr_mgr_nbr_entry_t *entry,cps_api_operation_types_t op){
    if(entry == NULL){
        NBR_MGR_LOG_ERR("NL_RESOLVE","Null ARP entry pointer passed to convert it to cps object");
        return NULL;
    }

    cps_api_object_t obj = cps_api_object_create();
    if(obj == NULL){
        NBR_MGR_LOG_ERR("NL_RESOLVE","Failed to allocate memory to cps object");
        return NULL;
    }

    cps_api_key_t key;
    cps_api_key_from_attr_with_qual(&key, BASE_ROUTE_OBJ_NBR, cps_api_qualifier_OBSERVED);
    cps_api_object_set_type_operation(&key,op);
    cps_api_object_set_key(obj,&key);
    if(entry->family == HAL_INET4_FAMILY){
        cps_api_object_attr_add_u32(obj,BASE_ROUTE_OBJ_NBR_ADDRESS,
                                    entry->nbr_addr.u.ipv4.s_addr);
    }else{
        cps_api_object_attr_add(obj,BASE_ROUTE_OBJ_NBR_ADDRESS,
                                (void *)entry->nbr_addr.u.ipv6.s6_addr,HAL_INET6_LEN);
    }
    char mac_addr[NBR_MGR_MAC_STR_LEN];
    memset(mac_addr, '\0', sizeof(mac_addr));
    std_mac_to_string(&(entry->nbr_hwaddr), mac_addr, NBR_MGR_MAC_STR_LEN);
    cps_api_object_attr_add(obj, BASE_ROUTE_OBJ_NBR_MAC_ADDR, (const void *)mac_addr,
                            strlen(mac_addr)+1);
    cps_api_object_attr_add_u32(obj,BASE_ROUTE_OBJ_NBR_VRF_ID,entry->vrfid);
    cps_api_object_attr_add_u32(obj,BASE_ROUTE_OBJ_NBR_AF,entry->family);
    cps_api_object_attr_add_u32(obj,BASE_ROUTE_OBJ_NBR_IFINDEX,entry->if_index);

    cps_api_object_attr_add_u32(obj,BASE_ROUTE_OBJ_NBR_FLAGS,entry->flags);
    if (entry->status & NBR_MGR_NUD_PERMANENT) {
        cps_api_object_attr_add_u32(obj,BASE_ROUTE_OBJ_NBR_TYPE,BASE_ROUTE_RT_TYPE_STATIC);
    } else {
        cps_api_object_attr_add_u32(obj,BASE_ROUTE_OBJ_NBR_TYPE,BASE_ROUTE_RT_TYPE_DYNAMIC);
    }
    return obj;
}

bool nbr_mgr_burst_resolve_handler(nbr_mgr_msg_t *p_msg) {
    char str[NBR_MGR_MAC_STR_LEN];
    char buff[HAL_INET6_TEXT_LEN + 1];
    NBR_MGR_LOG_INFO("NETLINK-MSG", "%s the neighbor family:%s ip:%s mac:%s"
                     " if-index:%d status:%d processing",
                     ((p_msg->type == NBR_MGR_NL_RESOLVE_MSG) ? "Resolve" :"Refresh"),
                     ((p_msg->nbr.family == HAL_INET4_FAMILY) ? "IPv4" : "IPv6"),
                     std_ip_to_string(&(p_msg->nbr.nbr_addr), buff, HAL_INET6_TEXT_LEN),
                     std_mac_to_string (&(p_msg->nbr.nbr_hwaddr), str,
                                        NBR_MGR_MAC_STR_LEN),
                     p_msg->nbr.if_index, p_msg->nbr.status);
    cps_api_object_t obj = nbr_mgr_nbr_to_cps_obj(&(p_msg->nbr), cps_api_oper_CREATE);
    if (obj == nullptr) {
        NBR_MGR_LOG_ERR("NETLINK-MSG", "Object creation failed for %s the neighbor family:%d ip:%s mac:%s"
                        " if-index:%d status:%d processing",
                        ((p_msg->type == NBR_MGR_NL_RESOLVE_MSG) ? "Resolve" :"Refresh"),
                        p_msg->nbr.family, std_ip_to_string(&(p_msg->nbr.nbr_addr),
                                                            buff, HAL_INET6_TEXT_LEN),
                        std_mac_to_string (&(p_msg->nbr.nbr_hwaddr), str,
                                           NBR_MGR_MAC_STR_LEN),
                        p_msg->nbr.if_index, p_msg->nbr.status);
        return false;
    }
    /* Invoke the NAS-linux APIs for Neighbor resolution and refresh */
    if (p_msg->type == NBR_MGR_NL_RESOLVE_MSG) {
        nas_os_resolve_neighbor(obj);
    } else if ((p_msg->type == NBR_MGR_NL_REFRESH_MSG) ||
               (p_msg->type == NBR_MGR_NL_DELAY_REFRESH_MSG)) {
        nas_os_refresh_neighbor(obj);
    }
    cps_api_object_delete(obj);
    return true;
}

bool nbr_mgr_nbr_resolve(nbr_mgr_msg_type_t type, nbr_mgr_nbr_entry_t *p_nbr) {
    nbr_mgr_msg_t *p_msg = nullptr;
    auto p_msg_uptr = nbr_mgr_alloc_unique_msg(&p_msg);
    if (p_msg) {
        p_msg->type = type;
        memcpy(&(p_msg->nbr), p_nbr, sizeof(nbr_mgr_nbr_entry_t));
        if (type == NBR_MGR_NL_DELAY_REFRESH_MSG) {
            nbr_mgr_enqueue_delay_resolve_msg(std::move(p_msg_uptr));
        } else {
            nbr_mgr_enqueue_burst_resolve_msg(std::move(p_msg_uptr));
        }
    }
    return true;
}

