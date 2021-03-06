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
 * filename: nbr_mgr_proc.cpp
 */

#include "nbr_mgr_main.h"
#include "nbr_mgr_cache.h"
#include "nbr_mgr_log.h"
#include "nbr_mgr_timer.h"
#include "std_thread_tools.h"
#include "std_mac_utils.h"
#include <exception>
#include <iostream>

static hal_mac_addr_t g_zero_mac = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static std_thread_create_param_t nbr_mgr_proc_thr;
nbr_process* p_nbr_process_hdl;
void nbr_proc_thread_main(void *ctx);
extern char *nbr_mgr_nl_neigh_state_to_str (int state);

bool nbr_mgr_process_main(void)
{
    try {
        p_nbr_process_hdl = new nbr_process;
        memset(&(p_nbr_process_hdl->stats), 0, sizeof(nbr_mgr_stats));

        std_thread_init_struct(&nbr_mgr_proc_thr);
        nbr_mgr_proc_thr.name = "nbr_mgr_proc_thr";
        nbr_mgr_proc_thr.thread_function = (std_thread_function_t)nbr_proc_thread_main;
        nbr_mgr_proc_thr.param = p_nbr_process_hdl;
        if (std_thread_create(&nbr_mgr_proc_thr)!=STD_ERR_OK) {
            throw std::runtime_error("Thread create failed");
        }
        return true;
    } catch (std::exception& e) {
        NBR_MGR_LOG_DEBUG("PROC", "Exception occurred %s", e.what());
    }

    return false;
}

void nbr_proc_thread_main(void *ctx) {
    auto proc_ptr = static_cast<nbr_process *> (ctx);

    proc_ptr->process_nbr_entries();
}

bool nbr_list_if_update(hal_vrf_id_t vrfid, hal_ifindex_t idx, hal_ifindex_t llayer_if_index, const nbr_data* ptr, bool op) {
    /* Update the higher layer (Router interface) on lower layer interface (L2)
     * for any flush/MAC deleted to trigger associated neighbor refresh. */

    if(op) {
        /* @@TODO Optimize this. */
        if (idx != llayer_if_index) {
            nbr_mgr_intf_entry_t intf;
            if (nbr_get_if_data(NBR_MGR_DEFAULT_VRF_ID, llayer_if_index, intf)) {
                NBR_MGR_LOG_INFO("IF-UPD", "Interface update - L3 VRF:%d intf:%d L2 intf:%d",
                                 vrfid, idx, llayer_if_index);
                if (intf.parent_or_child_if_index!= idx) {
                    intf.parent_or_child_vrfid= vrfid;
                    intf.parent_or_child_if_index = idx;
                    p_nbr_process_hdl->nbr_update_intf_info(intf);

                    if (nbr_get_if_data(vrfid, idx, intf)) {
                        intf.parent_or_child_vrfid = NBR_MGR_DEFAULT_VRF_ID;
                        intf.parent_or_child_if_index = llayer_if_index;

                        p_nbr_process_hdl->nbr_update_intf_info(intf);
                    }
                }
            }
        }
        p_nbr_process_hdl->nbr_list_if_add(vrfid, idx, ptr);
    } else {
        p_nbr_process_hdl->nbr_list_if_del(vrfid, idx, ptr);
    }
    return true;
}

bool nbr_get_if_data(hal_vrf_id_t vrfid, hal_ifindex_t idx, nbr_mgr_intf_entry_t& intf) {
    auto v_itr = p_nbr_process_hdl->neighbor_if_db.find(vrfid);
    if(v_itr == p_nbr_process_hdl->neighbor_if_db.end()) return false;

    auto& if_list = v_itr->second;
    auto if_itr = if_list.find(idx);
    if (if_itr == if_list.end()) return false;

    intf = if_itr->second;
    return true;
}

//Return the admin status of interface - true if up, false if down
static bool nbr_check_intf_up(hal_vrf_id_t vrfid, hal_ifindex_t idx) {
    nbr_mgr_intf_entry_t intf;
    if(nbr_get_if_data(vrfid, idx, intf))
        return (intf.is_admin_up);

    return false;
}

//Nbr process Class definitions
void nbr_process::process_nbr_entries(void)
{
    NBR_MGR_LOG_DEBUG("PROC", "Process main!");
    while(true) {
        nbr_mgr_msg_uptr_t pmsg = nbr_mgr_dequeue_netlink_nas_msg ();
        if (!pmsg) continue;

        auto& ptr = *pmsg.get();

        switch(ptr.type) {
            case NBR_MGR_NL_INTF_EVT:
                /* Intf message from NAS-linux for handling interface del
                 * and admin down events */
                nbr_proc_intf_msg(ptr.intf);
                break;
            case NBR_MGR_NL_NBR_EVT:
                /* IP neighbor message from NAS-linux for handling the nbr updates
                 * and programming the NPU, refresh the neighbors etc.*/
                nbr_proc_nbr_msg(ptr.nbr);
                break;
            case NBR_MGR_NL_MAC_EVT:
                /* MAC nbr message from NAS-linux for handling the FDB updates,
                 * 1. Refreshes the neighbor(s) on MAC delete
                 * 2. Helps with member port info. for IP Nbr NPU programming for bridge */
                nbr_proc_fdb_msg(ptr.nbr);
                break;
            case NBR_MGR_NAS_NBR_MSG:
                /* Neighbor message from NAS-L3 for proactive resolution
                 * (i.e resolve trigger to NAS-linux) */
                nbr_proc_nbr_msg(ptr.nbr);
                break;
            case NBR_MGR_NAS_FLUSH_MSG:
                nbr_proc_flush_msg(ptr.flush);
                break;
            case NBR_MGR_DUMP_MSG:
                nbr_proc_dump_msg(ptr.dump);
                break;
            default:
                NBR_MGR_LOG_DEBUG("PROC", "Unknown Message type");
        }
    }
}

mac_data_ptr nbr_process::mac_db_get(hal_ifindex_t ifidx, std::string& mac) {
    auto if_itr = mac_db.find(ifidx);
    if(if_itr == mac_db.end()) throw std::invalid_argument("Entry not found");

    auto& m_lst =  if_itr->second;

    auto itr = m_lst.find(mac);
    if(itr == m_lst.end()) throw std::invalid_argument("Entry not found");;

    return itr->second;
}

mac_data_ptr nbr_process::mac_db_get(hal_ifindex_t ifidx, const hal_mac_addr_t& mac_addr) {

    auto if_itr = mac_db.find(ifidx);
    if(if_itr == mac_db.end()) throw std::invalid_argument("Entry not found");

    auto& m_lst =  if_itr->second;

    std::string mac = nbr_mac_addr_string(mac_addr);
    NBR_MGR_LOG_DEBUG("PROC", "Mac %s, ifidx %d", mac.c_str(), ifidx);

    auto itr = m_lst.find(mac);
    if(itr == m_lst.end()) throw std::invalid_argument("Entry not found");;

    return itr->second;
}

mac_data_ptr nbr_process::mac_db_get_w_create(hal_ifindex_t ifidx, const hal_mac_addr_t& mac_addr) {

    std::string mac = nbr_mac_addr_string(mac_addr);

    NBR_MGR_LOG_DEBUG("PROC", "Mac %s, ifidx %d", mac.c_str(), ifidx);

    auto if_itr = mac_db.find(ifidx);
    if(if_itr == mac_db.end()) return create_mac_instance(mac, mac_addr, ifidx);

    auto& m_lst =  if_itr->second;

    auto itr = m_lst.find(mac);
    if(itr == m_lst.end()) return create_mac_instance(mac, mac_addr, ifidx);

    return itr->second;
}

mac_data_ptr nbr_process::mac_db_update(hal_ifindex_t ifidx, std::string& mac,
        mac_data_ptr& dptr) {

    mac_db[ifidx][mac] = std::move(dptr);

    return mac_db_get(ifidx, mac);
}

bool nbr_process::mac_db_remove(hal_ifindex_t ifidx, const std::string& mac)
{
    NBR_MGR_LOG_DEBUG("PROC", "Mac %s, ifidx %d", mac.c_str(), ifidx);

    auto if_itr = mac_db.find(ifidx);
    if(if_itr == mac_db.end()) throw std::invalid_argument("Entry not found");

    auto& m_lst =  if_itr->second;

    auto itr = m_lst.find(mac);
    if(itr == m_lst.end()) throw std::invalid_argument("Entry not found");;

    m_lst.erase(itr);

    if(m_lst.empty()) mac_db.erase(if_itr);

    return true;
}


bool nbr_process::nbr_proc_fdb_msg(const nbr_mgr_nbr_entry_t& entry) {

    std::string mac = nbr_mac_addr_string (entry.nbr_hwaddr);
    NBR_MGR_LOG_INFO("PROC", "FDB msg_type:%s, vrf-id:%lu, family:%s(%d), MAC:%s, "
                     "ifindex:%d/phy_idx:%d, expire:%lu, flags:0x%lx, status:%s",
                     ((entry.msg_type == NBR_MGR_NBR_ADD) ? "Add" : "Del"), entry.vrfid,
                     "Bridge", entry.family, mac.c_str(), entry.if_index, entry.mbr_if_index,
                     entry.expire, entry.flags, nbr_mgr_nl_neigh_state_to_str (entry.status));

    mac_data_ptr ptr;
    try {
        if(entry.msg_type == NBR_MGR_NBR_ADD) {
            ptr = mac_db_get_w_create(entry.if_index, entry.nbr_hwaddr);
            /* Publish the nbr data to NAS-L3 only if the ARP dependent
               new MAC is learnt or member port changed */
            stats.fdb_add_msg_cnt++;
            if ((ptr->get_fdb_type() == FDB_TYPE::FDB_LEARNED) &&
                (ptr->get_mac_phy_if() == entry.mbr_if_index)) {
                NBR_MGR_LOG_INFO("PROC", "FDB already learnt or refreshed!");
                return true;
            }
            if(entry.mbr_if_index) {
                ptr->update_mac_if(entry.mbr_if_index);
                ptr->set_fdb_type(FDB_TYPE::FDB_LEARNED);
            } else
                ptr->set_fdb_type(FDB_TYPE::FDB_IGNORE);
            NBR_MGR_LOG_DEBUG("PROC", "Fdb entry created/Updated");
        } else {
            ptr = mac_db_get(entry.if_index, entry.nbr_hwaddr);
            stats.fdb_del_msg_cnt++;

            if(ptr->nbr_list_empty())
                return delete_mac_instance(entry.nbr_hwaddr, entry.if_index);

            ptr->update_mac_if(0);
            ptr->set_fdb_type(FDB_TYPE::FDB_INCOMPLETE);
            NBR_MGR_LOG_DEBUG("PROC", "Fdb entry Updated");
        }
        //Invoking observer handlers
        ptr->for_each_nbr_list([&entry](const nbr_data *n_ptr, std::string mac) {
                               n_ptr->handle_fdb_change(entry.msg_type, entry.status);
                               });
        ptr->display();
    } catch(std::invalid_argument& e) {
        NBR_MGR_LOG_DEBUG("PROC", "%s", e.what());
        return false;
    } catch (std::bad_alloc& e){
        NBR_MGR_LOG_DEBUG("PROC", "%s", e.what());
        return false;
    }

    return true;
}

mac_data_ptr nbr_process::create_mac_instance(std::string& mac, const nbr_mgr_nbr_entry_t& entry) {

    std::shared_ptr<mac_data> data_ptr(new mac_data(entry));

    NBR_MGR_LOG_DEBUG("PROC", "Mac %s, ifidx %d, mbr index %d",
                       mac.c_str(), entry.if_index, entry.mbr_if_index);
    return mac_db_update(entry.if_index, mac, data_ptr);
}

mac_data_ptr nbr_process::create_mac_instance(std::string& mac,
                                              const hal_mac_addr_t& mac_addr, hal_ifindex_t ifidx) {

    NBR_MGR_LOG_DEBUG("PROC", "Mac %s, ifidx %d", mac.c_str(), ifidx);

    std::shared_ptr<mac_data> data_ptr(new mac_data(mac_addr, ifidx));

    return mac_db_update(ifidx, mac, data_ptr);
}

bool nbr_process::delete_mac_instance(const hal_mac_addr_t& mac_addr, hal_ifindex_t ifidx) {

    std::string mac = nbr_mac_addr_string(mac_addr);

    NBR_MGR_LOG_DEBUG("PROC", "Mac %s, ifidx %d", mac.c_str(), ifidx);

    return mac_db_remove(ifidx, mac);
}

bool nbr_process::delete_mac_instance(const std::string& mac, hal_ifindex_t ifidx) {

    NBR_MGR_LOG_DEBUG("PROC", "Mac %s, ifidx %d", mac.c_str(), ifidx);

    return mac_db_remove(ifidx, mac);
}

nbr_ip_db_type& nbr_process::neighbor_db(unsigned short family) noexcept {

    return ((family == HAL_INET4_FAMILY)?neighbor_db4:neighbor_db6);
}

nbr_data_ptr& nbr_process::nbr_db_get(nbr_ip_db_type& nbr_db, hal_vrf_id_t vrf, std::string& key) {
    auto v_itr = nbr_db.find(vrf);
    if(v_itr == nbr_db.end()) throw std::invalid_argument("Entry not found");

    auto& nbr_lst =  v_itr->second;

    auto itr = nbr_lst.find(key);
    if(itr == nbr_lst.end()) throw std::invalid_argument("Entry not found");

    return itr->second;
}

bool nbr_process::nbr_db_remove(nbr_ip_db_type& nbr_db, hal_vrf_id_t vrf, std::string& key) {
    auto v_itr = nbr_db.find(vrf);
    if(v_itr == nbr_db.end()) throw std::invalid_argument("Entry not found");

    auto& nbr_lst =  v_itr->second;

    auto itr = nbr_lst.find(key);
    if(itr == nbr_lst.end()) throw std::invalid_argument("Entry not found");

    /* Do not delete the entry if the entry is marked for tracking
     * Reset the flag if the entry *must* be deleted
     */
    if((itr->second->get_flags() & NBR_MGR_NBR_RESOLVE) == NBR_MGR_NBR_RESOLVE)
        return false;

    auto& mac_ptr = itr->second->get_mac_ptr();

    //mac_ptr would be invalid after the delete operation
    if(mac_ptr && mac_ptr->nbr_list_empty() && !mac_ptr->is_valid())
        delete_mac_instance(mac_ptr->get_mac_addr(), mac_ptr->get_mac_intf());

    nbr_lst.erase(itr);

    if(nbr_lst.empty()) nbr_db.erase(v_itr);

    return true;
}

inline std::string nbr_get_key(hal_ifindex_t ifix, const hal_ip_addr_t& ip) {
    return (std::to_string(ifix) + "-" + nbr_ip_addr_string (ip));
}

inline std::string nbr_get_key(hal_ifindex_t ifix, const std::string& ip) {
    return (std::to_string(ifix) + "-" + ip);
}

nbr_data_ptr& nbr_process::nbr_db_get_w_create(const nbr_mgr_nbr_entry_t& entry) {

    nbr_ip_db_type& nbr_db = neighbor_db(entry.family);

    std::string key = nbr_get_key(entry.if_index, entry.nbr_addr);

    auto v_itr = nbr_db.find(entry.vrfid);
    if(v_itr == nbr_db.end()) return create_nbr_instance(nbr_db, key, entry);

    auto& nbr_lst =  v_itr->second;

    auto itr = nbr_lst.find(key);
    if(itr == nbr_lst.end()) return create_nbr_instance(nbr_db, key, entry);
    return itr->second;
}

nbr_data_ptr& nbr_process::nbr_db_update(nbr_ip_db_type& nbr_db, hal_vrf_id_t vrf, std::string& key,
                                         std::unique_ptr<nbr_data>& dptr) {
    nbr_db[vrf][key] = std::move(dptr);
    return nbr_db_get(nbr_db, vrf, key);
}

bool nbr_process::nbr_proc_nbr_msg(nbr_mgr_nbr_entry_t& entry) {

    std::string ip = nbr_ip_addr_string (entry.nbr_addr);
    std::string mac = nbr_mac_addr_string (entry.nbr_hwaddr);
    std::string key = nbr_get_key(entry.if_index, ip);

    if((entry.status != NBR_MGR_NUD_DELAY) &&
       (entry.status != NBR_MGR_NUD_PROBE)) {
        NBR_MGR_LOG_INFO("PROC", "Neighbor msg_type:%s, vrf %lu(%s), family:%s, Nbr:%s, MAC:%s, "
                         "ifindex:%d, phy_idx:%d, expire:%lu, flags:0x%lx, status:%s",
                         (entry.msg_type == NBR_MGR_NBR_ADD) ? "Add" : "Del", entry.vrfid, entry.vrf_name,
                         ((entry.family == HAL_INET4_FAMILY) ? "IPv4" :
                          ((entry.family == HAL_INET6_FAMILY) ? "IPv6" : "Bridge")),
                         ip.c_str(), mac.c_str(), entry.if_index, entry.mbr_if_index,
                         entry.expire, entry.flags,
                         nbr_mgr_nl_neigh_state_to_str (entry.status));
    }
    //Use get_with_create if the entry is new
    if(entry.msg_type == NBR_MGR_NBR_ADD) {
        if (entry.flags & NBR_MGR_NBR_RESOLVE) {
            stats.nbr_rslv_add_msg_cnt++;
        } else {
            stats.nbr_add_msg_cnt++;
        }

        if (entry.status & NBR_MGR_NUD_INCOMPLETE)
            stats.nbr_add_incomplete_msg_cnt++;
        if (entry.status & NBR_MGR_NUD_REACHABLE)
            stats.nbr_add_reachable_msg_cnt++;
        if (entry.status & NBR_MGR_NUD_STALE)
            stats.nbr_add_stale_msg_cnt++;
        if (entry.status & NBR_MGR_NUD_DELAY)
            stats.nbr_add_delay_msg_cnt++;
        if (entry.status & NBR_MGR_NUD_PROBE)
            stats.nbr_add_probe_msg_cnt++;
        if (entry.status & NBR_MGR_NUD_FAILED)
            stats.nbr_add_failed_msg_cnt++;
        if (entry.status & NBR_MGR_NUD_PERMANENT)
            stats.nbr_add_permanaent_cnt++;

        try {
            /*
             * Common processing function closure, Used below for specific cases
             * Captures "this" pointer as well.
             */
            auto l_fn = [&](nbr_mgr_nbr_entry_t& entry, nbr_data_ptr& ptr) {
                /* If MAC changed for the neighbor, delete the nbr from the NPU and
                 * re-install back with the latest info. */
                if (ptr->handle_mac_change(entry)) {
                    /* There is a MAC change for the neighbor, if the MAC del is already received,
                     * delete the FDB entry now */
                    NBR_MGR_LOG_ERR("PROC", "Neighbor MAC changed vrf-id:%lu Nbr:%s, current MAC:%s, "
                                    "New MAC:%s current ifindex:%d, new ifindex:%d "
                                    "new status:%s published:%d ",entry.vrfid, ip.c_str(),
                                    ptr->get_mac_ptr()->get_mac_addr().c_str(),
                                    mac.c_str(), ptr->get_if_index(), entry.if_index,
                                    nbr_mgr_nl_neigh_state_to_str (entry.status), ptr->get_published());

                    if(ptr->get_mac_ptr()->nbr_list_empty() && !(ptr->get_mac_ptr()->is_valid())) {
                        delete_mac_instance(ptr->get_mac_ptr()->get_mac_addr(), ptr->get_mac_ptr()->get_mac_intf());
                    }
                    ptr->reset_mac_ptr();
                    entry.flags |= NBR_MGR_NBR_MAC_CHANGE;
                }
                if(ptr->get_mac_ptr() == nullptr) {
                    if (memcmp (&entry.nbr_hwaddr, &g_zero_mac, sizeof (hal_mac_addr_t))) {
                        auto mac_ptr = mac_db_get_w_create(entry.parent_if, entry.nbr_hwaddr);
                        ptr->update_mac_ptr(mac_ptr);
                        NBR_MGR_LOG_DEBUG("PROC", "Nbr if_index %d router-intf %d mac-data updated",
                                          entry.parent_if, entry.if_index);
                    }
                }
                ptr->display();
                ptr->process_nbr_data(entry);
            };

            //Do not create the entry with DELAY/PROBE/FAILED if not already present
            if((entry.status & NBR_MGR_NUD_DELAY) ||
               (entry.status & NBR_MGR_NUD_PROBE) ||
               (entry.status & NBR_MGR_NUD_FAILED)) {
                nbr_ip_db_type& nbr_db = neighbor_db(entry.family);
                auto &ptr = nbr_db_get(nbr_db, entry.vrfid, key);
                l_fn(entry, ptr);
            } else {
                if ((entry.status == NBR_MGR_NUD_INCOMPLETE) && !nbr_check_intf_up(entry.vrfid, entry.if_index))
                    return false;
                auto &ptr = nbr_db_get_w_create(entry);
                l_fn(entry, ptr);
            }

        } catch (std::bad_alloc& e) {
            NBR_MGR_LOG_ERR("PROC", "Memory allocation failure! %s", e.what());
            return false;
        } catch (std::invalid_argument& e) {
            NBR_MGR_LOG_DEBUG("PROC", "%s", e.what());
            return false;
        }
        return true;
    }

    if (entry.flags & NBR_MGR_NBR_RESOLVE) {
        stats.nbr_rslv_del_msg_cnt++;
    } else {
        stats.nbr_del_msg_cnt++;
    }

    /* Neighbor delete case handling */
    try {
        nbr_ip_db_type& nbr_db = neighbor_db(entry.family);
        auto& ptr = nbr_db_get(nbr_db, entry.vrfid, key);
        ptr->display();
        if (ptr->process_nbr_data(entry))
            nbr_db_remove(nbr_db, entry.vrfid, key);
    } catch (std::invalid_argument& e){
        NBR_MGR_LOG_ERR("PROC", "Nbr does not exist! %s", e.what());
        return false;
    }

    return true;
}

nbr_data_ptr& nbr_process::create_nbr_instance(nbr_ip_db_type& nbr_db, std::string& key,
                                               const nbr_mgr_nbr_entry_t& entry) {

    NBR_MGR_LOG_DEBUG("PROC", "Nbr %s, if_index %d",key.c_str(), entry.if_index);

    if (memcmp (&entry.nbr_hwaddr, &g_zero_mac, sizeof (hal_mac_addr_t))) {
        auto mac_ptr = mac_db_get_w_create(entry.parent_if, entry.nbr_hwaddr);
        std::unique_ptr<nbr_data> data_ptr(new nbr_data(entry, mac_ptr));
        return nbr_db_update(nbr_db, entry.vrfid, key, data_ptr);
    } else {
        std::unique_ptr<nbr_data> data_ptr(new nbr_data(entry));
        return nbr_db_update(nbr_db, entry.vrfid, key, data_ptr);
    }
}

bool nbr_process::delete_nbr_instance(nbr_ip_db_type& nbr_db, hal_vrf_id_t vrf, std::string key) {

    NBR_MGR_LOG_DEBUG("PROC", "Vrf %d,Nbr %s",vrf, key.c_str());
    return nbr_db_remove(nbr_db, vrf, key);
}

inline bool nbr_check_for_duplicate(nbr_mgr_intf_entry_t& intf1, nbr_mgr_intf_entry_t& intf2) {
    if((intf1.is_admin_up == intf2.is_admin_up) && (intf1.vlan_id == intf2.vlan_id)) return true;
    return false;
}

bool nbr_process::nbr_update_intf_info(nbr_mgr_intf_entry_t& intf) {
    NBR_MGR_LOG_INFO("PROC", "Intf update VRF-id:%lu is-del:%d, if_index:%d, flags:%d is_admin_up:%d,"
                     "is_bridge:%d vlan-id:%d", intf.vrfid, intf.is_op_del, intf.if_index,
                     intf.flags, intf.is_admin_up, intf.is_bridge, intf.vlan_id);

    neighbor_if_db[intf.vrfid][intf.if_index] = intf;
    return true;
}

bool nbr_process::nbr_proc_intf_msg(nbr_mgr_intf_entry_t& intf) {
    NBR_MGR_LOG_INFO("PROC", "Intf VRF-id:%lu is-del:%d, if_index:%d, flags:%d is_admin_up:%d,"
                     "is_bridge:%d vlan-id:%d", intf.vrfid, intf.is_op_del, intf.if_index,
                     intf.flags, intf.is_admin_up, intf.is_bridge, intf.vlan_id);

    if(!intf.is_op_del) {
        auto vrf_itr = neighbor_if_db.find(intf.vrfid);
        if (vrf_itr != neighbor_if_db.end()) {
            auto &intf_list = vrf_itr->second;

            auto itr = intf_list.find(intf.if_index);
            stats.intf_add_msg_cnt++;
            if(itr != intf_list.end()) {
                auto& intf_db = itr->second;
                /* Update the VLAN-id and return. */
                if (intf.flags == NBR_MGR_INTF_VLAN_MSG) {
                    intf_db.vlan_id = intf.vlan_id;
                    return true;
                    /* Admin status changed and VLAN already present,
                     * copy the VLAN-id and handle Admin status change */
                } else if ((intf.flags == NBR_MGR_INTF_ADMIN_MSG) &&
                           (intf_db.vlan_id)) {
                    intf.vlan_id = intf_db.vlan_id;
                }
                /* If both admin and VLAN are changed, just handle it here. */
                if(nbr_check_for_duplicate(itr->second, intf))
                    return false;
                intf.parent_or_child_vrfid = intf_db.parent_or_child_vrfid;
                intf.parent_or_child_if_index = intf_db.parent_or_child_if_index;
            }
        }
        nbr_mgr_notify_intf_status (NBR_MGR_OP_CREATE, intf);
        neighbor_if_db[intf.vrfid][intf.if_index] = intf;
    } else {
        nbr_mgr_notify_intf_status (NBR_MGR_OP_DELETE, intf);
        auto vrf_itr = neighbor_if_db.find(intf.vrfid);
        if (vrf_itr != neighbor_if_db.end()) {
            auto &intf_list = vrf_itr->second;
            auto itr = intf_list.find(intf.if_index);
            if(itr != intf_list.end()) {
                auto& intf_db = itr->second;
                if (intf_db.parent_or_child_if_index) {
                    nbr_mgr_intf_entry_t dep_intf;
                    if (nbr_get_if_data(intf_db.parent_or_child_vrfid, intf_db.parent_or_child_if_index,
                                        dep_intf)) {
                        NBR_MGR_LOG_INFO("IF-UPD", "Interface update - L3 VRF:%d intf:%d L2 VRF:%d intf:%d",
                                         intf.vrfid, intf.if_index, intf_db.parent_or_child_vrfid,
                                         intf_db.parent_or_child_if_index);
                        dep_intf.parent_or_child_vrfid = 0;
                        dep_intf.parent_or_child_if_index = 0;
                        neighbor_if_db[dep_intf.vrfid][dep_intf.if_index] = dep_intf;
                    }
                    intf_list.erase(itr);
                }
            }
            if(intf_list.empty()) {
                neighbor_if_db.erase(vrf_itr);
            }
        }
        stats.intf_del_msg_cnt++;
    }

    nbr_if_db_walk(intf.vrfid, intf.if_index, [&](nbr_data const * n_ptr) mutable {
        const_cast<nbr_data *>(n_ptr)->handle_if_state_change(intf);
        nbr_ip_db_type& nbr_db = neighbor_db(n_ptr->get_family());

        //If interface is deleted or down, delete the nbr instances
        if(intf.is_op_del || !intf.is_admin_up) {
            std::string key = nbr_get_key(n_ptr->get_if_index(), n_ptr->get_ip_addr());
            this->delete_nbr_instance(nbr_db, n_ptr->get_vrf_id(), key);
        }
    });

    return true;
}

bool nbr_process::nbr_list_if_add(hal_vrf_id_t vrfid, hal_ifindex_t ifidx, const nbr_data* ptr) {
    NBR_MGR_LOG_DEBUG("PROC", "Adding to if_db if_index %d, ptr 0x%p", ifidx, ptr);
    neighbor_if_nbr_db[vrfid][ifidx].insert(ptr);
    return true;
}

bool nbr_process::nbr_list_if_del(hal_vrf_id_t vrfid, hal_ifindex_t ifidx, const nbr_data* ptr) {
    NBR_MGR_LOG_DEBUG("PROC", "Del from if_db VRF:%d if_index %d, ptr 0x%p", vrfid, ifidx, ptr);
    auto v_itr = neighbor_if_nbr_db.find(vrfid);
    if (v_itr == neighbor_if_nbr_db.end()) return false;
    /* VRF->Intf-list */
    auto &if_list = v_itr->second;

    auto if_itr = if_list.find(ifidx);
    if(if_itr == if_list.end()) return false;
    /* VRF->Intf-list->Nbr(s) */

    if_list[ifidx].erase(ptr);
    if (if_list.empty())
        neighbor_if_nbr_db.erase(v_itr);
    return true;
}

bool nbr_process::nbr_proc_flush_msg(nbr_mgr_flush_entry_t& flush) {
    NBR_MGR_LOG_INFO("PROC", "FLUSH Intf:%d VRF-id:%d", flush.if_index, flush.vrfid);

    /* If the VRF is deleted, delete the interfaces and
     * the neighbors associated with that VRF. */
    if (flush.vrfid) {
        auto v_itr = neighbor_if_nbr_db.find(flush.vrfid);
        if (v_itr == neighbor_if_nbr_db.end()) return false;
        /* VRF->Intf-list */
        auto &if_list = v_itr->second;

        nbr_mgr_intf_entry_t intf;
        memset(&intf, 0, sizeof(intf));
        intf.vrfid = flush.vrfid;
        intf.is_op_del = true;
        for (auto ix = if_list.begin(), intf_ix = ix; ix != if_list.end() ;) {
            /* Store nbr to be deleted and then go for next nbr. */
            intf_ix = ix;
            ix++;
            intf.if_index = intf_ix->first;
            NBR_MGR_LOG_INFO("PROC", "VRF FLUSH Intf:%d VRF-id:%d", intf.if_index, flush.vrfid);

            nbr_proc_intf_msg(intf);
        }
        return true;
    }
    stats.flush_msg_cnt++;
    auto ln_f = [&](nbr_data const * n_ptr) mutable {
        if (!(n_ptr->get_status() & NBR_MGR_NUD_PERMANENT)) {
            if (((n_ptr->get_flags()) & NBR_MGR_NBR_REFRESH) ||
                (n_ptr->get_status() & NBR_MGR_NUD_INCOMPLETE)) {
                /* Neighbor is being refreshed now, increment the refresh count
                 * so as to refresh further once the current
                 * refresh in progress is done */
                this->stats.flush_nbr_cnt++;
                const_cast<nbr_data *>(n_ptr)->set_refresh_cnt();
                const_cast<nbr_data *>(n_ptr)->nbr_stats.flush_skip_refresh++;
            } else if (n_ptr->get_status() & NBR_MGR_NUD_FAILED) {
                /* If the neighbor is already in the failed state while refreshing,
                 * resolve the neighbor i.e send broadcast ARP request */
                this->stats.flush_trig_refresh_cnt++;
                const_cast<nbr_data *>(n_ptr)->trigger_resolve();
                const_cast<nbr_data *>(n_ptr)->nbr_stats.flush_failed_resolve++;
            } else {
                /* Refresh the neighbor by sending the unicast ARP request */
                this->stats.flush_trig_refresh_cnt++;
                const_cast<nbr_data *>(n_ptr)->trigger_refresh();
                const_cast<nbr_data *>(n_ptr)->nbr_stats.flush_refresh++;
            }
        }
    };

    if (flush.if_index) {
        /* Flush only interface case, refresh only the neighbors
         * associated with that interface */
        hal_vrf_id_t flush_vrf_id = NBR_MGR_DEFAULT_VRF_ID;
        hal_ifindex_t flush_if_index = flush.if_index;
        nbr_mgr_intf_entry_t llayer_intf;
        if (nbr_get_if_data(NBR_MGR_DEFAULT_VRF_ID, flush.if_index, llayer_intf)) {
            if (llayer_intf.parent_or_child_if_index != 0) {
                flush_vrf_id = llayer_intf.parent_or_child_vrfid;
                flush_if_index = llayer_intf.parent_or_child_if_index;
            }
        }
        NBR_MGR_LOG_INFO("PROC", "FLUSH Intf:%d hlayer info VRF:%d if-index:%d",
                         flush.if_index, flush_vrf_id, flush_if_index);
        nbr_if_db_walk(flush_vrf_id, flush_if_index, ln_f);
    } else {
        /* Flush all case, refresh all the neighbors */
        nbr_db_walk(ln_f);
    }

    return true;
}

//Walk routines

void nbr_process::nbr_db4_walk(std::function <void (nbr_data const *)> fn, hal_vrf_id_t vrf) {

    const auto& v_itr = neighbor_db4.find(vrf);
    if( v_itr == neighbor_db4.end()) return;

    auto& nbr_list_it = v_itr->second;
    for(auto const& ix : nbr_list_it)  fn(ix.second.get());
}

void nbr_process::nbr_db6_walk(std::function <void (nbr_data const *)> fn, hal_vrf_id_t vrf) {

    const auto& v_itr = neighbor_db6.find(vrf);
    if( v_itr == neighbor_db6.end()) return;

    auto& nbr_list_it = v_itr->second;
    for(auto const& ix : nbr_list_it)  fn(ix.second.get());
}

void nbr_process::nbr_db_walk(std::function <void (nbr_data const *)> fn) {
    for(auto& v_itr : neighbor_if_nbr_db) {
        auto& if_db = v_itr.second;
        for(auto& itr : if_db) {
            auto& l_itr = itr.second;
            for(auto ix: l_itr) fn(ix);
        }
    }
}

void nbr_process::nbr_if_db_walk(hal_vrf_id_t vrfid, hal_ifindex_t ifidx, std::function <void (nbr_data const *)> fn) {

    auto v_itr = neighbor_if_nbr_db.find(vrfid);
    if (v_itr == neighbor_if_nbr_db.end()) return;
    auto& if_list = v_itr->second;

    auto l_fn = [&](nbr_data_list &l_itr) {
        NBR_MGR_LOG_INFO("PROC", "VRF:%d if_index %d, nbr-count:%lu", vrfid, ifidx, l_itr.size());
        for (auto ix = l_itr.begin(), nbr_ix = ix; ix != l_itr.end() ;) {
            /* Store nbr to be deleted and then go for next nbr. */
            nbr_ix = ix;
            ix++;
            fn(*nbr_ix);
        }
    };

    /* Walk all interfaces-> Nbrs in the VRF */
    if (ifidx == 0) {
        for(auto& itr : if_list) {
            l_fn(itr.second);
        }
    } else {
        /* Walk interface-> Nbrs in the VRF */
        const auto itr = if_list.find(ifidx);
        if(itr == if_list.end()) return;

        l_fn(itr->second);
    }
}

void nbr_process::mac_db_walk(std::function <void (mac_data_ptr )> fn) {

    for(auto& itr : mac_db) {
        auto &l_itr = itr.second;
        for(auto ix: l_itr) fn(ix.second);
    }
}

void nbr_process::mac_if_db_walk(hal_ifindex_t ifidx, std::function <void (mac_data_ptr )> fn) {

    const auto if_itr = mac_db.find(ifidx);
    if(if_itr == mac_db.end()) return;

    auto& m_lst =  if_itr->second;

    for (auto ix = m_lst.begin(); ix != m_lst.end() ;) {
        auto mac_ix = ix;
        ix++;
        fn(mac_ix->second);
    }
}

void nbr_process::nbr_if_list_walk(std::function <void (nbr_mgr_intf_entry_t )> fn) {

    for(auto &v_itr : neighbor_if_db) {
        auto &if_itr = v_itr.second;
        for(auto &itr : if_itr) {
            fn(itr.second);
        }
    }
}

void nbr_process::nbr_if_list_entry_walk(hal_vrf_id_t vrf_id, hal_ifindex_t ifidx,
                                         std::function <void (nbr_mgr_intf_entry_t )> fn) {
    auto v_itr = neighbor_if_db.find(vrf_id);
    if(v_itr == neighbor_if_db.end()) return;

    auto& if_list = v_itr->second;

    if (ifidx) {
        auto if_itr = if_list.find(ifidx);
        if (if_itr == if_list.end()) return;
        fn(if_itr->second);
    } else {
        for (auto &if_itr : if_list)
            fn(if_itr.second);
    }
}


//Mac_data class definitions
mac_data::mac_data(const nbr_mgr_nbr_entry_t& mac_entry) {
    m_mac_addr = nbr_mac_addr_string(mac_entry.nbr_hwaddr);
    m_if_index = mac_entry.if_index;
    m_mbr_index = mac_entry.mbr_if_index;
    display();
}

mac_data::mac_data(const hal_mac_addr_t& mac_addr, hal_ifindex_t ifidx) {
    m_mac_addr = nbr_mac_addr_string(mac_addr);
    m_if_index = ifidx;
    m_mbr_index = 0;
    display();
}

void mac_data::update_mac_if(hal_ifindex_t port) noexcept {
    m_mbr_index = port;
}

void mac_data::display() const {
    NBR_MGR_LOG_DEBUG("PROC", "Mac data: HWaddr %s, if_index %d, mbr_index %d",
            m_mac_addr.c_str(), m_if_index, m_mbr_index);
}

//Nbr_data class definitions
void nbr_data::display() const {
    std::string ip = nbr_ip_addr_string (m_ip_addr);
    NBR_MGR_LOG_DEBUG("PROC", "vrf-id %d, family %d, Nbr %s, if_index %d, status %d, owner %d",
                      m_vrf_id, m_family, ip.c_str(), m_ifindex, m_status, m_flags);
}

bool nbr_data::process_nbr_data(nbr_mgr_nbr_entry_t& entry) {
    bool rc = true;
    std::string ip = nbr_ip_addr_string (m_ip_addr);
    NBR_MGR_LOG_DEBUG("PROC", "vrf %d(%s), family %d, Nbr %s, if_index %d, phy_idx %d, status %d,"
                      "flags %d entry type:%d status:%lu flags:%lu",
                      m_vrf_id, m_vrf_name.c_str(), m_family, ip.c_str(), m_ifindex, entry.mbr_if_index, m_status,
                      m_flags, entry.msg_type, entry.status, entry.flags);
    /*
     * Nbr entry already exists
     * 1. If Nbr add and status incomplete, program blackhole entry in the NPU.
     * 2. If Nbr add and status failed and proactive resolution is not desired,
     *    remove the entry in the NPU.
     *    Count the no. of attempts to resolve the Nbr, if reaches some threshold say 5 times,
     *    wait for say 20 secs before removing the blackhole entry in the NPU to give the fair
     *    share for other valid neighbors that need to resolved.
     *    @@TODO exponential timer based on no. of attempts to be explored.
     * 3. If Nbr_add and status reachable, program forward entry in the NPU.
     * 4. If Nbr add and status failed and proactive resolution desired,
     *    send message to kernel to resolve the nbr again.
     * 5. If Nbr add and status stale, refresh the neighbor
     *    by sending the message to kernel.
     */
    if(entry.msg_type == NBR_MGR_NBR_ADD) {
        /* Pro-active resolution desired by the NAS-L3, trigger resolution
         * 1. m_status 0(default) indicates that resolution is not triggered yet
         * 2. Resolution failed, trigger resolution again. */
        if (entry.flags & NBR_MGR_NBR_RESOLVE) {
            m_flags |= NBR_MGR_NBR_RESOLVE;
            if ((m_status == NBR_MGR_NUD_NONE) || (m_status & NBR_MGR_NUD_FAILED))
                rc = trigger_resolve();
            return rc;
        }

        /* Update the parent if not done already for the nbr triggered for pro-active resolution.
         * @@TODO Optimize this by getting the MAC-VLAN with parent interface in the interface event. */
        if ((m_parent_if == 0) && (entry.parent_if) && (m_ifindex != entry.parent_if)) {
            m_parent_if = entry.parent_if;

            nbr_mgr_intf_entry_t intf;
            if (nbr_get_if_data(NBR_MGR_DEFAULT_VRF_ID, m_parent_if, intf)) {
                NBR_MGR_LOG_INFO("IF-UPD", "Interface update - VRF:%d intf:%d L2 intf:%d",
                                 m_vrf_id, m_ifindex, m_parent_if);
                if (intf.parent_or_child_if_index != m_ifindex) {
                    intf.parent_or_child_vrfid = m_vrf_id;
                    intf.parent_or_child_if_index = m_ifindex;

                    p_nbr_process_hdl->nbr_update_intf_info(intf);
                    if (nbr_get_if_data(m_vrf_id, m_ifindex, intf)) {
                        intf.parent_or_child_vrfid = NBR_MGR_DEFAULT_VRF_ID;
                        intf.parent_or_child_if_index = m_parent_if;

                        p_nbr_process_hdl->nbr_update_intf_info(intf);
                    }
                }
            }
        }
        //In case if both incomplete and reachable set by kernel, treat it as reachable
        if (entry.status == (NBR_MGR_NUD_INCOMPLETE | NBR_MGR_NUD_REACHABLE))
            entry.status = NBR_MGR_NUD_REACHABLE;

        if (entry.status & NBR_MGR_NUD_INCOMPLETE) {
            /* Program blackhole entry in the NPU, Ignore the INCOMPLETE notification
             * after ARP refresh failures during NBR_MGR_MAX_NBR_RETRY_CNT */
            if (!(m_flags & NBR_MGR_NBR_REFRESH)) {
                rc = publish_entry(NBR_MGR_OP_CREATE, entry);
            }
        } else if ((entry.status & NBR_MGR_NUD_REACHABLE) ||
                   (entry.status & NBR_MGR_NUD_PERMANENT) ||
                   ((entry.status & NBR_MGR_NUD_DELAY) &&
                    (m_status & NBR_MGR_NUD_INCOMPLETE)) ||
                   ((entry.status & NBR_MGR_NUD_STALE) &&
                    (m_status & NBR_MGR_NUD_INCOMPLETE)) ||
                   ((m_status & NBR_MGR_NUD_FAILED) &&
                    (entry.status & NBR_MGR_NUD_STALE)) ||
                   ((entry.status & NBR_MGR_NUD_STALE) &&
                    ((!m_published) || (entry.flags & NBR_MGR_NBR_MAC_CHANGE)))) {

            /* Somtimes we get INCOMPLETE->DELAY directly instead of INCOMPLETE->REACHABLE,
             * so, programming the NPU in this also. */
            /* Somtimes we get INCOMPLETE->STALE directly instead of INCOMPLETE->REACHABLE,
             * so, programming the NPU in this also. */
            /* Mac pointer is present and check whether it is
             * updated based on FDB event from netlink
             * Incase of Gratuitous ARP, kernel the notifies the nbr entry with STALE state,
             * we have to differentiate this stale (new entry) from an existing entry,
             * and then program the NPU.
             * Sometimes during 64 path ECMP route case,
             * FAILED->STALE transition is happening, allowing this for HW programming
             */
            m_failed_cnt = 0;
            if(m_mac_data_ptr && m_mac_data_ptr->is_valid()) {
                /* Since the neighbor info. is unchanged (except the state) during ARP refresh,
                 * ignore the nbr msg notification to NAS-L3 */
                bool is_hw_mac_chk_req = false;
                if (m_flags & NBR_MGR_NBR_REFRESH) {
                    if (m_refresh_cnt) {
                        NBR_MGR_LOG_INFO("PROC", "Refresh again, vrf-id:%d(%s), family:%d,"
                                         "Nbr:%s, if_index:%d, phy_idx:%d, status:%d,"
                                         "flags:0x%x entry type:%d status:%lu flags:%lu m_refresh_cnt:%d",
                                         m_vrf_id, m_vrf_name.c_str(), m_family, ip.c_str(), m_ifindex,
                                         entry.mbr_if_index, m_status,
                                         m_flags, entry.msg_type, entry.status, entry.flags,
                                         m_refresh_cnt);
                    }
                    /* If refresh count is > 0, refresh the nbr again, this could be because
                     * while the refresh in progress, we could have received more flushes,
                     * refresh again */
                    if (m_refresh_cnt) {
                        reset_refresh_cnt();
                        trigger_refresh();
                    } else {
                        m_prev_refresh_for_mac_learn_retry_cnt = m_refresh_for_mac_learn_retry_cnt;
                        m_refresh_for_mac_learn_retry_cnt = 0;
                        m_flags &= ~NBR_MGR_NBR_REFRESH_FOR_MAC_LEARN;
                        m_flags &= ~NBR_MGR_NBR_REFRESH;
                        /* This publish is required only to update the application
                         * with the neighbor state changes */
                        publish_entry(NBR_MGR_OP_CREATE, entry);
                        is_hw_mac_chk_req = true;
                    }
                } else if (m_flags & NBR_MGR_NBR_REFRESH_FOR_MAC_LEARN) {
                    m_flags &= ~NBR_MGR_NBR_REFRESH_FOR_MAC_LEARN;
                    if (m_refresh_for_mac_learn_retry_cnt == NBR_MGR_MAX_NBR_REFRESH_MAC_LEARN_RETRY_CNT) {
                        NBR_MGR_LOG_ERR("MAC-LEARN", "MAC not learnt in the HW with the ARP refresh even "
                                        "after %d retries, vrf-id:%d(%s),"
                                        " family:%d, Nbr:%s, if_index:%d, phy_idx:%d, status:%d,"
                                        "flags:0x%x entry type:%d status:%lu flags:%lu",
                                        NBR_MGR_MAX_NBR_REFRESH_MAC_LEARN_RETRY_CNT,
                                        m_vrf_id, m_vrf_name.c_str(), m_family, ip.c_str(), m_ifindex,
                                        entry.mbr_if_index, m_status,
                                        m_flags, entry.msg_type, entry.status, entry.flags);
                        /* This publish is required only to update the application
                         * with the neighbor state changes */
                        publish_entry(NBR_MGR_OP_CREATE, entry);
                    } else {
                        m_refresh_for_mac_learn_retry_cnt++;
                        is_hw_mac_chk_req = true;
                    }
                } else {
                    entry.mbr_if_index = m_mac_data_ptr->get_mac_phy_if();
                    rc = publish_entry(NBR_MGR_OP_CREATE, entry);
                    is_hw_mac_chk_req = true;
                }
                if (is_hw_mac_chk_req) {
                    /* Check if the MAC is learnt from ARP response,
                     * if not, refresh until MAC learns in the NPU */
                    bool is_mac_present_in_hw = false;

                    if (nbr_mgr_is_mac_present_in_hw(entry.nbr_hwaddr,
                                                     entry.parent_if, is_mac_present_in_hw)) {
                        if (is_mac_present_in_hw == false) {
                            NBR_MGR_LOG_INFO("PROC", "MAC is not present in HW, "
                                            "Refreshing Nbr for vrf-id %d(%s), family %d, Nbr %s, "
                                            "if_index %d (ll-intf:%d), phy_idx %d, status %d,"
                                            "flags %d entry type:%d status:%lu flags:%lu refresh for MAC-learn:%d",
                                            m_vrf_id, m_vrf_name.c_str(), m_family, ip.c_str(), m_ifindex,
                                            m_parent_if, entry.mbr_if_index, m_status,
                                            m_flags, entry.msg_type, entry.status, entry.flags,
                                            m_refresh_for_mac_learn_retry_cnt);
                            nbr_stats.mac_not_present_cnt++;
                            trigger_refresh_for_mac_learn();
                        } else {
                            NBR_MGR_LOG_INFO("PROC", "MAC is present in HW, "
                                             "Nbr for vrf-id %d(%s), family %d, Nbr %s, flags:0x%x"
                                             "if_index %d (ll-intf:%d), phy_idx %d, status %d,"
                                             "flags %d entry type:%d status:%lu flags:%lu refresh for MAC-learn:%d",
                                             m_vrf_id, m_vrf_name.c_str(), m_family, ip.c_str(), m_flags,
                                             m_ifindex, m_parent_if, entry.mbr_if_index, m_status,
                                             m_flags, entry.msg_type, entry.status, entry.flags,
                                             m_refresh_for_mac_learn_retry_cnt);
                            m_flags &= ~NBR_MGR_NBR_REFRESH_FOR_MAC_LEARN;
                            m_prev_refresh_for_mac_learn_retry_cnt = m_refresh_for_mac_learn_retry_cnt;
                            m_refresh_for_mac_learn_retry_cnt = 0;
                        }
                    }
                }
                m_retry_cnt = 0;
            } else if (entry.status & NBR_MGR_NUD_PERMANENT) {
                m_flags |= NBR_MGR_MAC_NOT_PRESENT;
            } else {
                /* MAC data ptr is not populated, could be VLT type scenario,
                 * try refresh few times and then wait for MAC event and
                 * program the host in the NPU
                 */
                nbr_stats.retry_cnt++;
                if (m_retry_cnt == NBR_MGR_MAX_NBR_RETRY_CNT) {
                    NBR_MGR_LOG_INFO("PROC", "MAC not learnt with the ARP refresh, vrf-id:%d(%s),"
                                    " family:%d, Nbr:%s, if_index:%d (ll-intf:%d), phy_idx:%d, status:%d,"
                                    "flags:0x%x entry type:%d status:%lu flags:%lu",
                                    m_vrf_id, m_vrf_name.c_str(), m_family, ip.c_str(), m_ifindex,
                                    m_parent_if, entry.mbr_if_index, m_status,
                                    m_flags, entry.msg_type, entry.status, entry.flags);
                } else {
                    if (!(m_flags & NBR_MGR_NBR_REFRESH))
                        m_flags |= NBR_MGR_MAC_NOT_PRESENT;

                    m_retry_cnt++;
                    /* If the MAC is not learnt, it could be because of
                     * the MAC learning disable on the L3 interface, wait for APP to program the MAC */
                    rc = trigger_delay_refresh();
                    NBR_MGR_LOG_INFO("PROC", "MAC data is not present, vrf-id:%d(%s), family:%d,"
                                     "Nbr:%s, if_index:%d (ll-intf:%d), phy_idx:%d, status:%d,"
                                     "flags:0x%x entry type:%d status:%lu flags:%lu",
                                     m_vrf_id, m_vrf_name.c_str(), m_family, ip.c_str(), m_ifindex,
                                     m_parent_if, entry.mbr_if_index, m_status,
                                     m_flags, entry.msg_type, entry.status, entry.flags);
                }
            }
            if (m_mac_data_ptr && (m_flags & NBR_MGR_MAC_NOT_PRESENT)) {
                /* Dont wait for MAC to learn in the kernel, program the NPU since
                 * SAI accepts the host creation even if the associated MAC is not learnt
                 * in the NPU, but refresh the nbr until the MAC learns in the kernel,
                 * this ensures the MAC learning in the NPU for unidirectional traffic case */
                entry.mbr_if_index = m_mac_data_ptr->get_mac_phy_if();
                rc = publish_entry(NBR_MGR_OP_CREATE, entry);
            }
        } else if (entry.status & NBR_MGR_NUD_FAILED) {
            m_retry_cnt = 0;
            m_flags &= ~NBR_MGR_NBR_REFRESH_FOR_MAC_LEARN;
            m_prev_refresh_for_mac_learn_retry_cnt = m_refresh_for_mac_learn_retry_cnt;
            m_refresh_for_mac_learn_retry_cnt = 0;
            if (m_flags & NBR_MGR_NBR_REFRESH) {
                /* Since there can be a topology settling issue,
                 * try the refresh/resolve for retry_cnt and
                 * if fails, remove the nbr entry in the NPU to lift
                 * the packets for ARP resolution */
                nbr_stats.failed_trig_resolve_cnt++;
                if (m_failed_cnt == NBR_MGR_MAX_NBR_RETRY_CNT) {
                    NBR_MGR_LOG_INFO("PROC", " Not resolved even after multiple re-tries vrf:%d(%s),"
                                     " family:%d,Nbr:%s, intf:%d, phy_idx:%d, status:%d,"
                                     "flags %d entry type:%d status:%lu flags:%lu m_refresh_cnt:%d",
                                     m_vrf_id, m_vrf_name.c_str(), m_family, ip.c_str(), m_ifindex, entry.mbr_if_index,
                                     m_status, m_flags, entry.msg_type, entry.status, entry.flags,
                                     m_refresh_cnt);
                    /* If refresh count is > 0, refresh the nbr again, this could be because
                     * while the refresh in progress, we could have received more flushes,
                     * refresh again */
                    if (m_refresh_cnt) {
                        reset_refresh_cnt();
                        trigger_resolve();
                    } else {
                        m_flags &= ~NBR_MGR_NBR_REFRESH;
                        if (m_flags & NBR_MGR_NBR_RESOLVE)
                            trigger_resolve();

                        rc = publish_entry(NBR_MGR_OP_CREATE, entry);
                    }
                } else {
                    /* Reset the refresh count during trigger resolve on failure case */
                    reset_refresh_cnt();
                    m_failed_cnt++;
                    rc = trigger_resolve();
                }
            } else if (m_flags & NBR_MGR_NBR_RESOLVE) {
                trigger_resolve();
                rc = publish_entry(NBR_MGR_OP_CREATE, entry);
            } else {
                /* If Nbr flushed, delete the nbr entry from the NPU.*/
                rc = publish_entry(NBR_MGR_OP_CREATE, entry);
            }
        }
        if (entry.status & NBR_MGR_NUD_STALE) {
            /* Check whether the auto refresh is enabled for the neighbor */
            if (entry.auto_refresh_on_stale_enabled) {
                /* If the current is incomplete, dont set for refresh */
                if (!(m_status & NBR_MGR_NUD_INCOMPLETE)) {
                    m_flags |= NBR_MGR_NBR_REFRESH;
                }
                nbr_stats.stale_trig_refresh_cnt++;
                rc = trigger_refresh();
            }
            /* This publish is required only to update the application
             * with the neighbor state changes */
            publish_entry(NBR_MGR_OP_CREATE, entry);
        }
        m_status = entry.status;
    } else if(entry.msg_type == NBR_MGR_NBR_DEL) {
        if (entry.flags & NBR_MGR_NBR_RESOLVE) {
            if (!(m_flags & NBR_MGR_NBR_RESOLVE)) {
                NBR_MGR_LOG_ERR("PROC", "Err - Unexpected msg vrf-id %d(%s), family %d, "
                                "Nbr %s, if_index %d, phy_idx %d, status %d,"
                                "flags %d entry type:%d status:%lu flags:%lu",
                                m_vrf_id, m_vrf_name.c_str(), m_family, ip.c_str(), m_ifindex,
                                entry.mbr_if_index, m_status, m_flags,
                                entry.msg_type, entry.status, entry.flags);
                return true;
            }
            /* Nbr stop resolve update from NAS-L3 -
             * This neighbor is no longer needs to be proactively resolved */
            m_flags &= ~NBR_MGR_NBR_RESOLVE;

            /* On reset of nbr resolve flag from nas-l3,
             * this neighbor no longer needs to be refreshed
             */
            m_flags &= ~NBR_MGR_NBR_REFRESH;
            /* If the nbr is in initial state for some reason, delete the nbr entry */
            if (m_status == NBR_MGR_NUD_NONE)
                return true;
            /* The neighbor is in non-default state (non-zero), there may be other
             * applications, dont remove the neighbor */
            return false;
        }
        if (m_flags & NBR_MGR_NBR_RESOLVE) {
            /* If NH needs to be pro-actively resolved but kernel notified delete,
             * resolve the NH again and if the NH is in forward state in NPU, delete the entry */
            m_status = NBR_MGR_NUD_NONE;
            trigger_resolve();
            rc = publish_entry(NBR_MGR_OP_DELETE, entry);
            if (rc == false) {
                NBR_MGR_LOG_ERR("PROC", "Err - Publish failed vrf-id %d, family %d, "
                                "Nbr %s, if_index %d, phy_idx %d, status %d,"
                                "flags %d entry type:%d status:%lu flags:%lu",
                                m_vrf_id, m_family, ip.c_str(), m_ifindex,
                                entry.mbr_if_index, m_status, m_flags,
                                entry.msg_type, entry.status, entry.flags);
            }
            /* Dont delete the neighbor if proactive resolution is desired */
            return false;
        }
        rc = publish_entry(NBR_MGR_OP_DELETE, entry);
    }
    return rc;
}

void nbr_data::populate_nbr_entry(nbr_mgr_nbr_entry_t& entry) const {
    entry.vrfid = m_vrf_id;
    entry.family = m_family;
    memcpy(&entry.nbr_addr, &m_ip_addr, sizeof(entry.nbr_addr));
    entry.if_index = m_ifindex;
    strncpy(entry.vrf_name, m_vrf_name.c_str(), sizeof(entry.vrf_name));
    entry.status = m_status;
    if (m_mac_data_ptr) {
        std_string_to_mac(&entry.nbr_hwaddr, m_mac_data_ptr->get_mac_addr().c_str(),
                          sizeof(entry.nbr_hwaddr));
        if(m_mac_data_ptr->is_valid())
            entry.mbr_if_index = m_mac_data_ptr->get_mac_phy_if();
    }
}

bool nbr_data::handle_fdb_change(nbr_mgr_evt_type_t evt, unsigned long status) const {
    std::string ip = nbr_ip_addr_string (m_ip_addr);
    NBR_MGR_LOG_DEBUG("PROC-NL-MSG", "Handle FDB change for %s if-index:%d",
                      ip.c_str(), m_ifindex);

    /* On FDB del (may be because of STP topology change), refresh the ARPs/Nbrs
     * to re-learn on correct port using ARP/NA reply incase of uni-directional
     * traffic case */
    if ((evt == NBR_MGR_NBR_DEL) || (status & NBR_MGR_NUD_STALE)) {
        /* If the neighbor is dynamic, refresh the nbr on MAC delete
         * On MAC entry status STALE, refresh the nbr in order to refresh the MAC.
         * If nbr is already in INCOMPLETE/FAILED state, then no need to refresh nbr when MAC
         * is deleted or becomes stale */
        if (!(m_status & NBR_MGR_NUD_PERMANENT) &&
            !(m_status & NBR_MGR_NUD_FAILED) &&
            !(m_status & NBR_MGR_NUD_INCOMPLETE)) {
            m_flags |= NBR_MGR_NBR_REFRESH;
            nbr_stats.mac_trig_refresh++;
            return trigger_refresh();
        }
        /* @@TODO need to take some action on MAC delete for the static Nbr entry? */
        return true;
    } else if ((evt == NBR_MGR_NBR_ADD) && (m_status & NBR_MGR_NUD_FAILED)) {
        /* If the MAC learnt when the nbr is in failed state, refresh the nbr. */
        m_flags |= NBR_MGR_NBR_REFRESH;
        trigger_resolve();
    }

    nbr_mgr_nbr_entry_t nbr;
    memset(&nbr, 0, sizeof(nbr));
    nbr.msg_type = NBR_MGR_NBR_ADD;
    populate_nbr_entry(nbr);

    if (m_flags & NBR_MGR_MAC_NOT_PRESENT) {
        /* MAC learnt for Nbr, program the Nbr into the NPU */
        m_flags &= ~NBR_MGR_MAC_NOT_PRESENT;
        m_retry_cnt = 0;
        if (!((m_status & NBR_MGR_NUD_REACHABLE) ||
              (m_status & NBR_MGR_NUD_STALE) ||
              (m_status & NBR_MGR_NUD_DELAY) ||
              (m_status & NBR_MGR_NUD_PERMANENT))) {
            NBR_MGR_LOG_INFO("PROC-NL-MSG", "FDB change ignored for nbr:%s if-index:%d status:%d",
                            ip.c_str(), m_ifindex, m_status);
            return true;
        }
    } else {
         if ((!(m_status & NBR_MGR_NUD_PERMANENT)) &&
            (m_retry_cnt > 0)) {
            /* Refresh has been attempted because Nbr learnt before MAC learning,
             * refresh cnt is incremented, reset the count and REFRESH flag */
            m_retry_cnt = 0;
            m_flags &= ~NBR_MGR_NBR_REFRESH;
        }
        /* Program the Nbr dependent MAC into the NPU */
        //if(m_mac_data_ptr && (m_mac_data_ptr->get_fdb_type() != FDB_TYPE::FDB_IGNORE))
        //    nbr.status = NBR_MGR_NUD_NOARP;
        /* Dont program the MAC into the NPU, SAI would take care of that */
        return true;
    }

    return publish_entry(NBR_MGR_OP_UPDATE, nbr);
}

bool nbr_data::handle_if_state_change(nbr_mgr_intf_entry_t& intf) {

    std::string ip = nbr_ip_addr_string (m_ip_addr);
    NBR_MGR_LOG_DEBUG("PROC-IF-CHG", "Handle IF state change for %s if-index:%d "
                      "admin %d, m_flags 0x%x, m_status 0x%x ",
                      ip.c_str(), intf.if_index, intf.is_admin_up, m_flags, m_status);
    if(intf.is_admin_up) {
        if((m_flags & NBR_MGR_NBR_RESOLVE) &&
            !(m_status & NBR_MGR_NUD_REACHABLE) &&
            !(m_status & NBR_MGR_NUD_PERMANENT))
            trigger_resolve();
    } else
        m_status = NBR_MGR_NUD_NONE;

    return true;
}

bool nbr_data::handle_mac_change(nbr_mgr_nbr_entry_t& entry) {
    if (get_mac_ptr() == nullptr) {
        return false;
    }
    hal_mac_addr_t mac_addr;
    std_string_to_mac(&mac_addr, get_mac_ptr() ->get_mac_addr().c_str(),
                      sizeof(hal_mac_addr_t));
    /* Check if the nbr has learnt with different MAC */
    if ((memcmp (&entry.nbr_hwaddr, &mac_addr, sizeof (hal_mac_addr_t)) == 0) ||
        (memcmp (&mac_addr, &g_zero_mac, sizeof (hal_mac_addr_t)) == 0) ||
        (memcmp (&entry.nbr_hwaddr, &g_zero_mac, sizeof (hal_mac_addr_t)) == 0)) {
        return false;
    }

    if (m_published) {
        /* If NBR is not waiting for the MAC i.e NBR is already programmed,
         * delete the Nbr and re-program the Nbr with new MAC */
        nbr_mgr_nbr_entry_t nbr;
        memset(&nbr, 0, sizeof(nbr));
        populate_nbr_entry(nbr);
        nbr.msg_type = NBR_MGR_NBR_DEL;
        publish_entry(NBR_MGR_OP_DELETE, nbr);
    }
    delete_mac_ptr();

    return true;;
}

bool nbr_data::trigger_resolve() const {
    nbr_mgr_nbr_entry_t nbr;
    std::string ip = nbr_ip_addr_string (m_ip_addr);
    NBR_MGR_LOG_DEBUG("PROC-NL-MSG", "Resolve the neighbor %s if-index:%d",
                     ip.c_str(), m_ifindex);

    memset(&nbr, 0, sizeof(nbr));
    populate_nbr_entry(nbr);

    //Skip resolve if the interface is admin down/not exist
    if(!nbr_check_intf_up(nbr.vrfid, nbr.if_index))
        return false;

    nbr_stats.resolve_cnt++;
    /* Resolve the neighbor */
    return nbr_mgr_nbr_resolve(NBR_MGR_NL_RESOLVE_MSG, &nbr);
}

bool nbr_data::trigger_refresh() const {
    nbr_mgr_nbr_entry_t nbr;

    /* Dont refresh for static ARP entry */
    if (m_status & NBR_MGR_NUD_PERMANENT)
        return false;

    std::string ip = nbr_ip_addr_string (m_ip_addr);
    if (m_mac_data_ptr == NULL) {
        NBR_MGR_LOG_INFO("PROC", "MAC is NULL, refresh failed for Nbr %s",
                        ip.c_str());
        return false;
    }
    NBR_MGR_LOG_DEBUG("PROC-NL-MSG", "Refresh the neighbor %s MAC:%s if-index:%d",
                     ip.c_str(), m_mac_data_ptr->get_mac_addr().c_str(), m_ifindex);
    memset(&nbr, 0, sizeof(nbr));
    populate_nbr_entry(nbr);

    //Skip refresh if the interface is admin down/not exist
    if(!nbr_check_intf_up(nbr.vrfid, nbr.if_index))
        return false;

    if (!(m_flags & NBR_MGR_MAC_NOT_PRESENT))
        m_flags |= NBR_MGR_NBR_REFRESH;

    nbr_stats.refresh_cnt++;
    /* Refresh the neighbor */
    return nbr_mgr_nbr_resolve(NBR_MGR_NL_REFRESH_MSG, &nbr);
}

bool nbr_data::trigger_delay_refresh() const {
    nbr_mgr_nbr_entry_t nbr;

    /* Dont refresh for static ARP entry */
    if (m_status & NBR_MGR_NUD_PERMANENT)
        return false;

    std::string ip = nbr_ip_addr_string (m_ip_addr);
    if (m_mac_data_ptr == NULL) {
        NBR_MGR_LOG_INFO("PROC", "MAC is NULL, refresh failed for Nbr %s",
                        ip.c_str());
        return false;
    }
    NBR_MGR_LOG_DEBUG("PROC-NL-MSG", "Refresh the neighbor %s MAC:%s if-index:%d",
                     ip.c_str(), m_mac_data_ptr->get_mac_addr().c_str(), m_ifindex);
    memset(&nbr, 0, sizeof(nbr));
    populate_nbr_entry(nbr);

    //Skip refresh if the interface is admin down/not exist
    if(!nbr_check_intf_up(nbr.vrfid, nbr.if_index))
        return false;

    if (!(m_flags & NBR_MGR_MAC_NOT_PRESENT))
        m_flags |= NBR_MGR_NBR_REFRESH;

    nbr_stats.delay_refresh_cnt++;
    /* Refresh the neighbor */
    return nbr_mgr_nbr_resolve(NBR_MGR_NL_DELAY_REFRESH_MSG, &nbr);
}

bool nbr_data::trigger_refresh_for_mac_learn() const {
    nbr_mgr_nbr_entry_t nbr;

    /* Dont refresh for static ARP entry */
    if (m_status & NBR_MGR_NUD_PERMANENT)
        return false;

    std::string ip = nbr_ip_addr_string (m_ip_addr);
    if (m_mac_data_ptr == NULL) {
        NBR_MGR_LOG_INFO("PROC", "MAC is NULL, refresh failed for Nbr %s",
                        ip.c_str());
        return false;
    }
    NBR_MGR_LOG_INFO("PROC-NL-MSG", "Delay refresh the neighbor %s MAC:%s learning in the NPU if-index:%d",
                     ip.c_str(), m_mac_data_ptr->get_mac_addr().c_str(), m_ifindex);
    memset(&nbr, 0, sizeof(nbr));
    populate_nbr_entry(nbr);

    //Skip refresh if the interface is admin down/not exist
    if(!nbr_check_intf_up(nbr.vrfid, nbr.if_index))
        return false;

    m_flags |= NBR_MGR_NBR_REFRESH_FOR_MAC_LEARN;

    nbr_stats.hw_mac_learn_refresh_cnt++;
    /* Refresh the neighbor */
    return nbr_mgr_nbr_resolve(NBR_MGR_NL_DELAY_REFRESH_MSG, &nbr);
}


bool nbr_data::publish_entry(nbr_mgr_op_t op, const nbr_mgr_nbr_entry_t& entry) const {

    //If already published, modify operation to UPDATE
    if(m_published && op == NBR_MGR_OP_CREATE) op = NBR_MGR_OP_UPDATE;

    if(nbr_mgr_program_npu(op, entry)) m_published = true;

    nbr_stats.npu_prg_msg_cnt++;
    std::string ip = nbr_ip_addr_string (entry.nbr_addr);
    std::string mac = nbr_mac_addr_string (entry.nbr_hwaddr);
    m_last_status_published = entry.status;
    NBR_MGR_LOG_INFO("PROC", "Publish OP:%s msg_type:%s, vrf %lu(%s), family:%s, Nbr:%s, MAC:%s, "
                     "ifindex:%d, lower-layer-if:%d phy_idx:%d, expire:%lx, flags:0x%lx, status:%s nbr-vrf %d(%s) published:%d",
                     ((op == NBR_MGR_OP_CREATE) ? "Create" : ((op == NBR_MGR_OP_UPDATE)?
                                                              "Update" : "Del")),
                     (entry.msg_type == NBR_MGR_NBR_ADD) ? "Add" : "Del", entry.vrfid, entry.vrf_name,
                     ((entry.family == HAL_INET4_FAMILY) ? "IPv4" :
                      ((entry.family == HAL_INET6_FAMILY) ? "IPv6" : "Bridge")),
                     ip.c_str(), mac.c_str(), entry.if_index, entry.parent_if, entry.mbr_if_index,
                     entry.expire, entry.flags,
                     nbr_mgr_nl_neigh_state_to_str (entry.status), m_vrf_id, m_vrf_name.c_str(), m_published);

    return m_published;
}

