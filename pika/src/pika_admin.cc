// Copyright (c) 2015-present, Qihoo, Inc.  All rights reserved.
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree. An additional grant
// of patent rights can be found in the PATENTS file in the same directory.
#include <sys/time.h>
#include <iomanip>

#include "slash/include/slash_string.h"
#include "slash/include/rsync.h"
#include "pika_conf.h"
#include "pika_admin.h"
#include "pika_server.h"
#include "pika_slot.h"
#include "pika_version.h"
#include "build_version.h"
#include "pika_define.h"

#include <sys/utsname.h>
#ifdef TCMALLOC_EXTENSION
#include <gperftools/malloc_extension.h>
#endif

extern PikaServer *g_pika_server;
extern PikaConf *g_pika_conf;

void SlaveofCmd::DoInitial(PikaCmdArgsType &argv, const CmdInfo* const ptr_info) {
    if (!ptr_info->CheckArg(argv.size())) {
        res_.SetRes(CmdRes::kWrongNum, kCmdNameSlaveof);
        return;
    }
    PikaCmdArgsType::iterator it = argv.begin() + 1; //Remember the first args is the opt name

    master_ip_ = slash::StringToLower(*it++);

    is_noone_ = false;
    if (master_ip_ == "no" && slash::StringToLower(*it) == "one") {
        if (argv.end() - it == 1) {
            is_noone_ = true;
        } else {
            res_.SetRes(CmdRes::kWrongNum, kCmdNameSlaveof);
        }
        return;
    }

    std::string str_master_port = *it++;
    if (!slash::string2l(str_master_port.data(), str_master_port.size(), &master_port_) || master_port_ <= 0) {
        res_.SetRes(CmdRes::kInvalidInt);
        return;
    }

    if ((master_ip_ == "127.0.0.1" || master_ip_ == g_pika_server->host()) && master_port_ == g_pika_server->port()) {
        res_.SetRes(CmdRes::kErrOther, "you fucked up");
        return;
    }

    have_offset_ = false;
    int cur_size = argv.end() - it;
    if (cur_size == 0) {

    } else if (cur_size == 1) {
        std::string command = *it++;
        if (command != "force") {
            res_.SetRes(CmdRes::kSyntaxErr);
            return;
        }
        g_pika_server->SetForceFullSync(true);
    } else if (cur_size == 2) {
        have_offset_ = true;
        std::string str_filenum = *it++;
        if (!slash::string2l(str_filenum.data(), str_filenum.size(), &filenum_) || filenum_ < 0) {
            res_.SetRes(CmdRes::kInvalidInt);
            return;
        }
        std::string str_pro_offset = *it++;
        if (!slash::string2l(str_pro_offset.data(), str_pro_offset.size(), &pro_offset_) || pro_offset_ < 0) {
            res_.SetRes(CmdRes::kInvalidInt);
            return;
        }
    } else {
        res_.SetRes(CmdRes::kWrongNum, kCmdNameSlaveof);
    }
}

void SlaveofCmd::Do() {
    // Check if we are already connected to the specified master
    if ((master_ip_ == "127.0.0.1" || g_pika_server->master_ip() == master_ip_) &&
        g_pika_server->master_port() == master_port_) {
        res_.SetRes(CmdRes::kOk);
        return;
    }

    // Stop rsync
    LOG(INFO) << "start slaveof, stop rsync first";
    slash::StopRsync(g_pika_conf->db_sync_path());
    g_pika_server->RemoveMaster();

    if (is_noone_) {
        g_pika_conf->SetSlaveof("");
        if (g_pika_conf->disable_auto_compactions()) {
            g_pika_conf->SetDisableAutoCompactions(false);
            g_pika_server->db()->ResetOption("disable_auto_compactions", "false");
        }
        g_pika_server->SetForceFullSync(false);
        g_pika_conf->ConfigRewrite();
        res_.SetRes(CmdRes::kOk);
        return;
    } else {
        g_pika_conf->SetSlaveof(master_ip_ + ":" + std::to_string(master_port_));
        g_pika_conf->ConfigRewrite();
    }

    if (have_offset_) {
        // Before we send the trysync command, we need purge current logs older than the sync point
        if (filenum_ > 0) {
            g_pika_server->PurgeLogs(filenum_ - 1, true, true);
        }
        g_pika_server->logger_->SetProducerStatus(filenum_, pro_offset_);
    }
    bool sm_ret = g_pika_server->SetMaster(master_ip_, master_port_);
    if (sm_ret) {
        res_.SetRes(CmdRes::kOk);

        // clear cache when in slave model
        LOG(INFO) << "clear cache when change master to slave";
        g_pika_server->ClearCacheDbAsync();
    } else {
        res_.SetRes(CmdRes::kErrOther, "Server is not in correct state for slaveof");
    }
}

void TrysyncCmd::DoInitial(PikaCmdArgsType &argv, const CmdInfo* const ptr_info) {
    if (!ptr_info->CheckArg(argv.size())) {
        res_.SetRes(CmdRes::kWrongNum, kCmdNameTrysync);
        return;
    }
    PikaCmdArgsType::iterator it = argv.begin() + 1; //Remember the first args is the opt name
    slave_ip_ = *it++;

    std::string str_slave_port = *it++;
    if (!slash::string2l(str_slave_port.data(), str_slave_port.size(), &slave_port_) || slave_port_ <= 0) {
        res_.SetRes(CmdRes::kInvalidInt);
        return;
    }

    std::string str_filenum = *it++;
    if (!slash::string2l(str_filenum.data(), str_filenum.size(), &filenum_) || filenum_ < 0) {
        res_.SetRes(CmdRes::kInvalidInt);
        return;
    }

    std::string str_pro_offset = *it++;
    if (!slash::string2l(str_pro_offset.data(), str_pro_offset.size(), &pro_offset_) || pro_offset_ < 0) {
        res_.SetRes(CmdRes::kInvalidInt);
        return;
    }
}

void TrysyncCmd::Do() {
    LOG(INFO) << "Trysync, Slave ip: " << slave_ip_ << " Slave port:" << slave_port_
        << " filenum: " << filenum_ << " pro_offset: " << pro_offset_;
    int64_t sid = g_pika_server->TryAddSlave(slave_ip_, slave_port_);
    if (sid >= 0) {
        Status status = g_pika_server->AddBinlogSender(slave_ip_, slave_port_,
                filenum_, pro_offset_);
        if (status.ok()) {
            res_.AppendInteger(sid);
            LOG(INFO) << "Send Sid to Slave: " << sid;
            g_pika_server->BecomeMaster();
            return;
        }
        // Create Sender failed, delete the slave
        g_pika_server->DeleteSlave(slave_ip_, slave_port_);

        if (status.IsIncomplete()) {
            res_.AppendString(kInnerReplWait);
        } else {
            LOG(WARNING) << "slave offset is larger than mine, slave ip: " << slave_ip_
                << "slave port:" << slave_port_
                << " filenum: " << filenum_ << " pro_offset_: " << pro_offset_;
            res_.SetRes(CmdRes::kErrOther, "InvalidOffset");
        }
    } else {
        LOG(WARNING) << "slave already exist, slave ip: " << slave_ip_
            << "slave port: " << slave_port_;
        res_.SetRes(CmdRes::kErrOther, "AlreadyExist");
    }
}

void AuthCmd::DoInitial(PikaCmdArgsType &argv, const CmdInfo* const ptr_info) {
    if (!ptr_info->CheckArg(argv.size())) {
        res_.SetRes(CmdRes::kWrongNum, kCmdNameAuth);
        return;
    }
    pwd_ = argv[1];
}

void AuthCmd::Do() {
    std::string root_password(g_pika_conf->requirepass());
    std::string user_password(g_pika_conf->userpass());
    if (user_password.empty() && root_password.empty()) {
        res_.SetRes(CmdRes::kErrOther, "Client sent AUTH, but no password is set");
        return;
    }

    if (pwd_ == user_password) {
        res_.SetRes(CmdRes::kOk, "USER");
    }
    if (pwd_ == root_password) {
        res_.SetRes(CmdRes::kOk, "ROOT");
    }
    if (res_.none()) {
        res_.SetRes(CmdRes::kInvalidPwd);
    }
}

void BgsaveCmd::DoInitial(PikaCmdArgsType &argv, const CmdInfo* const ptr_info) {
    if (!ptr_info->CheckArg(argv.size())) {
        res_.SetRes(CmdRes::kWrongNum, kCmdNameBgsave);
        return;
    }
}
void BgsaveCmd::Do() {
    g_pika_server->Bgsave();
    const PikaServer::BGSaveInfo& info = g_pika_server->bgsave_info();
    char buf[256];
    snprintf(buf, sizeof(buf), "+%s : %u: %lu",
            info.s_start_time.c_str(), info.filenum, info.offset);
    res_.AppendContent(buf);
}

void BgsaveoffCmd::DoInitial(PikaCmdArgsType &argv, const CmdInfo* const ptr_info) {
    if (!ptr_info->CheckArg(argv.size())) {
        res_.SetRes(CmdRes::kWrongNum, kCmdNameBgsaveoff);
        return;
    }
}
void BgsaveoffCmd::Do() {
    CmdRes::CmdRet ret;
    if (g_pika_server->Bgsaveoff()) {
     ret = CmdRes::kOk;
    } else {
     ret = CmdRes::kNoneBgsave;
    }
    res_.SetRes(ret);
}

void CompactCmd::DoInitial(PikaCmdArgsType &argv, const CmdInfo* const ptr_info) {
    if (!ptr_info->CheckArg(argv.size())) {
        res_.SetRes(CmdRes::kWrongNum, kCmdNameCompact);
        return;
    }
}

void CompactCmd::Do() {
    rocksdb::Status s;
    s = g_pika_server->db()->Compact(blackwidow::kAll);
    if (s.ok()) {
        res_.SetRes(CmdRes::kOk);
    } else {
        res_.SetRes(CmdRes::kErrOther, s.ToString());
    }
}

void PurgelogstoCmd::DoInitial(PikaCmdArgsType &argv, const CmdInfo* const ptr_info) {
    if (!ptr_info->CheckArg(argv.size())) {
        res_.SetRes(CmdRes::kWrongNum, kCmdNamePurgelogsto);
        return;
    }
    std::string filename = slash::StringToLower(argv[1]);
    if (filename.size() <= kBinlogPrefixLen ||
            kBinlogPrefix != filename.substr(0, kBinlogPrefixLen)) {
        res_.SetRes(CmdRes::kInvalidParameter);
        return;
    }
    std::string str_num = filename.substr(kBinlogPrefixLen);
    int64_t num = 0;
    if (!slash::string2l(str_num.data(), str_num.size(), &num) || num < 0) {
        res_.SetRes(CmdRes::kInvalidParameter);
        return;
    }
    num_ = num;
}
void PurgelogstoCmd::Do() {
    if (g_pika_server->PurgeLogs(num_, true, false)) {
        res_.SetRes(CmdRes::kOk);
    } else {
        res_.SetRes(CmdRes::kPurgeExist);
    }
}

void PingCmd::DoInitial(PikaCmdArgsType &argv, const CmdInfo* const ptr_info) {
    if (!ptr_info->CheckArg(argv.size())) {
        res_.SetRes(CmdRes::kWrongNum, kCmdNamePing);
        return;
    }
}
void PingCmd::Do() {
    res_.SetRes(CmdRes::kPong);
}

void SelectCmd::DoInitial(PikaCmdArgsType &argv, const CmdInfo* const ptr_info) {
    if (!ptr_info->CheckArg(argv.size())) {
        res_.SetRes(CmdRes::kWrongNum, kCmdNameSelect);
        return;
    }

    int64_t db_id;
    if (!slash::string2l(argv[1].data(), argv[1].size(), &db_id) ||
            db_id < 0 || db_id > 15) {
        res_.SetRes(CmdRes::kInvalidIndex);
    }
}
void SelectCmd::Do() {
    res_.SetRes(CmdRes::kOk);
}

void FlushallCmd::DoInitial(PikaCmdArgsType &argv, const CmdInfo* const ptr_info) {
    if (!ptr_info->CheckArg(argv.size())) {
        res_.SetRes(CmdRes::kWrongNum, kCmdNameFlushall);
        return;
    }
}

void FlushallCmd::Do() {
    g_pika_server->RWLockWriter();
    if (g_pika_server->FlushAll()) {
        res_.SetRes(CmdRes::kOk);
    } else {
        res_.SetRes(CmdRes::kErrOther, "There are some bgthread using db now, can not flushall");
    }
    g_pika_server->RWUnlock();
}

void FlushallCmd::CacheDo() {
    Do();
}

void FlushallCmd::PostDo() {
    // clear cache
    if (PIKA_CACHE_NONE != g_pika_conf->cache_model()) {
        g_pika_server->ClearCacheDbAsync();
    }
}

void ReadonlyCmd::DoInitial(PikaCmdArgsType &argv, const CmdInfo* const ptr_info) {
    if (!ptr_info->CheckArg(argv.size())) {
        res_.SetRes(CmdRes::kWrongNum, kCmdNameReadonly);
        return;
    }
    std::string opt = slash::StringToLower(argv[1]);
    if (opt == "on" || opt == "1") {
        is_open_ = true;
    } else if (opt == "off" || opt == "0") {
        is_open_ = false;
    } else {
        res_.SetRes(CmdRes::kSyntaxErr, kCmdNameReadonly);
        return;
    }
}
void ReadonlyCmd::Do() {
    g_pika_server->RWLockWriter();
    if (is_open_) {
        g_pika_conf->SetReadonly(true);
    } else {
        g_pika_conf->SetReadonly(false);
    }
    res_.SetRes(CmdRes::kOk);
    g_pika_server->RWUnlock();
}

const std::string ClientCmd::CLIENT_LIST_S = "list";
const std::string ClientCmd::CLIENT_KILL_S = "kill";
void ClientCmd::DoInitial(PikaCmdArgsType &argv, const CmdInfo* const ptr_info) {
    if (!ptr_info->CheckArg(argv.size())) {
        res_.SetRes(CmdRes::kWrongNum, kCmdNameClient);
        return;
    }
    slash::StringToLower(argv[1]);
    if (argv[1] == CLIENT_LIST_S && argv.size() == 2) {
        //nothing
    } else if (argv[1] == CLIENT_KILL_S && argv.size() == 3) {
        ip_port_ = slash::StringToLower(argv[2]);
    } else {
        res_.SetRes(CmdRes::kErrOther, "Syntax error, try CLIENT (LIST | KILL ip:port)");
        return;
    }
    operation_ = argv[1];
    return;
}

void ClientCmd::Do() {
    if (operation_ == CLIENT_LIST_S) {
        struct timeval now;
        gettimeofday(&now, NULL);
        std::vector<ClientInfo> clients;
        g_pika_server->ClientList(&clients);
        std::vector<ClientInfo>::iterator iter= clients.begin();
        std::string reply = "";
        char buf[128];
        while (iter != clients.end()) {
            snprintf(buf, sizeof(buf), "addr=%s fd=%d idle=%ld\n", iter->ip_port.c_str(), iter->fd, iter->last_interaction == 0 ? 0 : now.tv_sec - iter->last_interaction);
            reply.append(buf);
            iter++;
        }
        res_.AppendString(reply);
    } else if (operation_ == CLIENT_KILL_S && ip_port_ == "all") {
        g_pika_server->ClientKillAll();
        res_.SetRes(CmdRes::kOk);
    } else if (g_pika_server->ClientKill(ip_port_) == 1) {
        res_.SetRes(CmdRes::kOk);
    } else {
        res_.SetRes(CmdRes::kErrOther, "No such client");
    }
    return;
}

void ShutdownCmd::DoInitial(PikaCmdArgsType &argv, const CmdInfo* const ptr_info) {
    if (!ptr_info->CheckArg(argv.size())) {
        res_.SetRes(CmdRes::kWrongNum, kCmdNameShutdown);
        return;
    }
}
// no return
void ShutdownCmd::Do() {
    DLOG(WARNING) << "handle \'shutdown\'";
    g_pika_server->Exit();
    res_.SetRes(CmdRes::kNone);
}

const std::string InfoCmd::kAllSection = "all";
const std::string InfoCmd::kServerSection = "server";
const std::string InfoCmd::kClientsSection = "clients";
const std::string InfoCmd::kStatsSection = "stats";
const std::string InfoCmd::kReplicationSection = "replication";
const std::string InfoCmd::kKeyspaceSection = "keyspace";
const std::string InfoCmd::kLogSection = "log";
const std::string InfoCmd::kDataSection = "data";
const std::string InfoCmd::kCache = "cache";

void InfoCmd::DoInitial(PikaCmdArgsType &argv, const CmdInfo* const ptr_info) {
    (void)ptr_info;
    size_t argc = argv.size();
    if (argc > 3) {
        res_.SetRes(CmdRes::kSyntaxErr);
        return;
    }
    if (argc == 1) {
        info_section_ = kInfoAll;
        return;
    } //then the agc is 2 or 3
    slash::StringToLower(argv[1]);
    if (argv[1] == kAllSection) {
        info_section_ = kInfoAll;
    } else if (argv[1] == kServerSection) {
        info_section_ = kInfoServer;
    } else if (argv[1] == kClientsSection) {
        info_section_ = kInfoClients;
    } else if (argv[1] == kStatsSection) {
        info_section_ = kInfoStats;
    } else if (argv[1] == kReplicationSection) {
        info_section_ = kInfoReplication;
    } else if (argv[1] == kKeyspaceSection) {
        info_section_ = kInfoKeyspace;
        if (argc == 2) {
            return;
        }
        if (argv[2] == "1") { //info keyspace [ 0 | 1 | off ]
            rescan_ = true;
        } else if (argv[2] == "off") {
            off_ = true;
        } else if (argv[2] != "0") {
            res_.SetRes(CmdRes::kSyntaxErr);
        }
        return;
    } else if (argv[1] == kLogSection) {
        info_section_ = kInfoLog;
    } else if (argv[1] == kDataSection) {
        info_section_ = kInfoData;
    } else if (argv[1] == kCache) {
        info_section_ = kInfoCache;
    } else {
        info_section_ = kInfoErr;
    }
    if (argc != 2) {
        res_.SetRes(CmdRes::kSyntaxErr);
    }
}

void InfoCmd::Do() {
    std::string info;
    switch (info_section_) {
        case kInfoAll:
            InfoServer(info);
            info.append("\r\n");
            InfoData(info);
            info.append("\r\n");
            InfoLog(info);
            info.append("\r\n");
            InfoClients(info);
            info.append("\r\n");
            InfoStats(info);
            info.append("\r\n");
            InfoReplication(info);
            info.append("\r\n");
            InfoKeyspace(info);
            info.append("\r\n");
            InfoCache(info);
            break;
        case kInfoServer:
            InfoServer(info);
            break;
        case kInfoClients:
            InfoClients(info);
            break;
        case kInfoStats:
            InfoStats(info);
            break;
        case kInfoReplication:
            InfoReplication(info);
            break;
        case kInfoKeyspace:
            InfoKeyspace(info);
            // off_ should return +OK
            if (off_) {
                res_.SetRes(CmdRes::kOk);
            }
            break;
        case kInfoLog:
            InfoLog(info);
            break;
        case kInfoData:
            InfoData(info);
            break;
        case kInfoCache:
            InfoCache(info);
            break;
        default:
            //kInfoErr is nothing
            break;
    }


    res_.AppendStringLen(info.size());
    res_.AppendContent(info);
    return;
}

void InfoCmd::InfoServer(std::string &info) {
    static struct utsname host_info;
    static bool host_info_valid = false;
    if (!host_info_valid) {
        uname(&host_info);
        host_info_valid = true;
    }

    time_t current_time_s = time(NULL);
    std::stringstream tmp_stream;
    char version[32];
    snprintf(version, sizeof(version), "%d.%d.%d-%d.%d", PIKA_MAJOR,
            PIKA_MINOR, PIKA_PATCH, PIKA_XMLY_MAJOR, PIKA_XMLY_MINOR);
    tmp_stream << "# Server\r\n";
    tmp_stream << "pika_version:" << version << "\r\n";
    tmp_stream << pika_build_git_sha << "\r\n";
    tmp_stream << "pika_build_compile_date: " <<
        pika_build_compile_date << "\r\n";
    tmp_stream << "os:" << host_info.sysname << " " << host_info.release << " " << host_info.machine << "\r\n";
    tmp_stream << "arch_bits:" << (reinterpret_cast<char*>(&host_info.machine) + strlen(host_info.machine) - 2) << "\r\n";
    tmp_stream << "process_id:" << getpid() << "\r\n";
    tmp_stream << "tcp_port:" << g_pika_conf->port() << "\r\n";
    tmp_stream << "thread_num:" << g_pika_conf->thread_num() << "\r\n";
    tmp_stream << "sync_thread_num:" << g_pika_conf->sync_thread_num() << "\r\n";
    tmp_stream << "uptime_in_seconds:" << (current_time_s - g_pika_server->start_time_s()) << "\r\n";
    tmp_stream << "uptime_in_days:" << (current_time_s / (24*3600) - g_pika_server->start_time_s() / (24*3600) + 1) << "\r\n";
    tmp_stream << "config_file:" << g_pika_conf->conf_path() << "\r\n";

    info.append(tmp_stream.str());
}

void InfoCmd::InfoClients(std::string &info) {
    std::stringstream tmp_stream;
    tmp_stream << "# Clients\r\n";
    tmp_stream << "connected_clients:" << g_pika_server->ClientList() << "\r\n";

    info.append(tmp_stream.str());
}

void InfoCmd::InfoStats(std::string &info) {
    std::stringstream tmp_stream;
    tmp_stream << "# Stats\r\n";

    tmp_stream << "total_connections_received:" << g_pika_server->accumulative_connections() << "\r\n";
    tmp_stream << "instantaneous_ops_per_sec:" << g_pika_server->ServerCurrentQps() << "\r\n";
    tmp_stream << "total_commands_processed:" << g_pika_server->ServerQueryNum() << "\r\n";
    PikaServer::BGSaveInfo bgsave_info = g_pika_server->bgsave_info();
    bool is_bgsaving = g_pika_server->bgsaving();
    time_t current_time_s = time(NULL);
    tmp_stream << "is_bgsaving:" << (is_bgsaving ? "Yes, " : "No, ") << bgsave_info.s_start_time << ", "
                                                                << (is_bgsaving ? (current_time_s - bgsave_info.start_time) : 0) << "\r\n";
    PikaServer::BGSlotsReload bgslotsreload_info = g_pika_server->bgslots_reload();
    bool is_reloading = g_pika_server->GetSlotsreloading();
    tmp_stream << "is_slots_reloading:" << (is_reloading ? "Yes, " : "No, ") << bgslotsreload_info.s_start_time << ", "
                                                                << (is_reloading ? (current_time_s - bgslotsreload_info.start_time) : 0) << "\r\n";
    PikaServer::BGSlotsDel bg_slots_del_info = g_pika_server->bgslots_del();
    bool is_slots_deleting = bg_slots_del_info.deleting;
    tmp_stream << "is_slots_deleting:" << (is_slots_deleting ? "Yes, " : "No, ") << bg_slots_del_info.s_start_time << ", "
                                << (is_slots_deleting ? (current_time_s - bg_slots_del_info.start_time) : 0) << ", slotno["
                                << bg_slots_del_info.slot_no << "], total: " << bg_slots_del_info.total << ", del: " << bg_slots_del_info.current << "\r\n";
    PikaServer::KeyScanInfo key_scan_info = g_pika_server->key_scan_info();
    bool is_scaning = g_pika_server->key_scaning();
    tmp_stream << "is_scaning_keyspace:" << (is_scaning ? ("Yes, " + key_scan_info.s_start_time) + "," : "No");
    if (is_scaning) {
        tmp_stream << current_time_s - key_scan_info.start_time;
    }
    tmp_stream << "\r\n";
    tmp_stream << "is_compact:" << g_pika_server->db()->GetCurrentTaskType() << "\r\n";
    tmp_stream << "compact_cron:" << g_pika_conf->compact_cron() << "\r\n";
    tmp_stream << "compact_interval:" << g_pika_conf->compact_interval() << "\r\n";

    info.append(tmp_stream.str());
}

void InfoCmd::InfoReplication(std::string &info) {
    int host_role = g_pika_server->role();
    std::stringstream tmp_stream;
    tmp_stream << "# Replication(";
    switch (host_role) {
        case PIKA_ROLE_SINGLE :
        case PIKA_ROLE_MASTER : tmp_stream << "MASTER)\r\nrole:master\r\n"; break;
        case PIKA_ROLE_SLAVE : tmp_stream << "SLAVE)\r\nrole:slave\r\n"; break;
        case PIKA_ROLE_MASTER | PIKA_ROLE_SLAVE : tmp_stream << "MASTER/SLAVE)\r\nrole:slave\r\n"; break;
        default: info.append("ERR: server role is error\r\n"); return;
    }

    std::string slaves_list_str;
    //int32_t slaves_num = g_pika_server->GetSlaveListString(slaves_list_str);
    switch (host_role) {
        case PIKA_ROLE_SLAVE :
            tmp_stream << "master_host:" << g_pika_server->master_ip() << "\r\n";
            tmp_stream << "master_port:" << g_pika_server->master_port() << "\r\n";
            tmp_stream << "master_link_status:" << (g_pika_server->repl_state() == PIKA_REPL_CONNECTED ? "up" : "down") << "\r\n";
            if(g_pika_server->repl_state() != PIKA_REPL_CONNECTED){
                time_t now = time(NULL);
                tmp_stream << "master_link_down_since_seconds:" << now - g_pika_server->repl_down_since() << "\r\n";
            }
            tmp_stream << "slave_priority:" << g_pika_conf->slave_priority() << "\r\n";
            tmp_stream << "slave_read_only:" << g_pika_conf->readonly() << "\r\n";
            tmp_stream << "repl_state: " << (g_pika_server->repl_state()) << "\r\n";
            break;
        case PIKA_ROLE_MASTER | PIKA_ROLE_SLAVE :
            tmp_stream << "master_host:" << g_pika_server->master_ip() << "\r\n";
            tmp_stream << "master_port:" << g_pika_server->master_port() << "\r\n";
            tmp_stream << "master_link_status:" << (g_pika_server->repl_state() == PIKA_REPL_CONNECTED ? "up" : "down") << "\r\n";
            if(g_pika_server->repl_state() != PIKA_REPL_CONNECTED){
                time_t now = time(NULL);
                tmp_stream << "master_link_down_since_seconds:" << now - g_pika_server->repl_down_since() << "\r\n";
            }
            tmp_stream << "slave_read_only:" << g_pika_conf->readonly() << "\r\n";
            tmp_stream << "repl_state: " << (g_pika_server->repl_state()) << "\r\n";
        case PIKA_ROLE_SINGLE :
        case PIKA_ROLE_MASTER :
            tmp_stream << "connected_slaves:" << g_pika_server->GetSlaveListString(slaves_list_str) << "\r\n" << slaves_list_str;
    }

    info.append(tmp_stream.str());
}

void InfoCmd::InfoKeyspace(std::string &info) {
    if (off_) {
        g_pika_server->StopKeyScan();
        off_ = false;
        return;
    }

    PikaServer::KeyScanInfo key_scan_info = g_pika_server->key_scan_info();
    std::vector<uint64_t> &key_nums_v = key_scan_info.key_nums_v;
    if (key_scan_info.key_nums_v.size() != 6) {
        info.append("info keyspace error\r\n");
        return;
    }
    std::stringstream tmp_stream;
    tmp_stream << "# Keyspace\r\n";
    tmp_stream << "db0:keys=" << (key_nums_v[0] + key_nums_v[1] + key_nums_v[2] + key_nums_v[3] + key_nums_v[4] + key_nums_v[5]) << "\r\n";
    tmp_stream << "# Time:" << key_scan_info.s_start_time << "\r\n";
    tmp_stream << "kv keys:" << key_nums_v[0] << "\r\n";
    tmp_stream << "hash keys:" << key_nums_v[1] << "\r\n";
    tmp_stream << "list keys:" << key_nums_v[2] << "\r\n";
    tmp_stream << "zset keys:" << key_nums_v[3] << "\r\n";
    tmp_stream << "set keys:" << key_nums_v[4] << "\r\n";
    tmp_stream << "ehash keys:" << key_nums_v[5] << "\r\n";
    info.append(tmp_stream.str());

    if (rescan_) {
        g_pika_server->KeyScan();
    }
    return;
}

void InfoCmd::InfoLog(std::string &info) {

    std::stringstream  tmp_stream;
    tmp_stream << "# Log" << "\r\n";
    uint32_t purge_max;
    int64_t log_size = g_pika_server->log_size_;

    tmp_stream << "log_size:" << log_size << "\r\n";
    tmp_stream << "log_size_human:" << (log_size >> 20) << "M\r\n";
    tmp_stream << "safety_purge:" << (g_pika_server->GetPurgeWindow(purge_max) ?
            kBinlogPrefix + std::to_string(static_cast<int32_t>(purge_max)) : "none") << "\r\n";
    tmp_stream << "expire_logs_days:" << g_pika_conf->expire_logs_days() << "\r\n";
    tmp_stream << "expire_logs_nums:" << g_pika_conf->expire_logs_nums() << "\r\n";
    uint32_t filenum;
    uint64_t offset;
    g_pika_server->logger_->GetProducerStatus(&filenum, &offset);
    tmp_stream << "binlog_offset:" << filenum << " " << offset << "\r\n";

    info.append(tmp_stream.str());
    return;
}

void InfoCmd::InfoData(std::string &info) {

    std::stringstream tmp_stream;
    int64_t db_size = g_pika_server->db_size_;
    // rocksdb related memory usage
    uint64_t memtable_usage = g_pika_server->memtable_usage_;
    uint64_t table_reader_usage = g_pika_server->table_reader_usage_;
    uint64_t cache_usage = g_pika_server->cache_usage_;

    tmp_stream << "# Data" << "\r\n";
    tmp_stream << "db_size:" << db_size << "\r\n";
    tmp_stream << "db_size_human:" << (db_size >> 20) << "M\r\n";
    tmp_stream << "compression:" << g_pika_conf->compression() << "\r\n";

    tmp_stream << "used_memory:" << (memtable_usage + table_reader_usage + cache_usage) << "\r\n";
    tmp_stream << "used_memory_human:" << ((memtable_usage + table_reader_usage + cache_usage) >> 20) << "M\r\n";
    tmp_stream << "db_memtable_usage:" << memtable_usage << "\r\n";
    tmp_stream << "db_tablereader_usage:" << table_reader_usage << "\r\n";
    tmp_stream << "cache_usage:" << cache_usage << "\r\n";

    info.append(tmp_stream.str());
    return;
}

void InfoCmd::InfoCache(std::string &info)
{
    PikaServer::DisplayCacheInfo cache_info;
    g_pika_server->GetCacheInfo(cache_info);

    std::stringstream tmp_stream;
    tmp_stream << "# Cache" << "\r\n";
    tmp_stream << "cache_status:" << cache_info.status << "\r\n";
    tmp_stream << "cache_db_num:" << cache_info.cache_num << "\r\n";
    tmp_stream << "cache_keys:" << cache_info.keys_num << "\r\n";
    tmp_stream << "cache_memory:" << cache_info.used_memory << "\r\n";
    tmp_stream << "cache_memory_human:" << (cache_info.used_memory >> 20) << "M\r\n";
    tmp_stream << "hits:" << cache_info.hits << "\r\n";
    tmp_stream << "all_cmds:" << cache_info.hits + cache_info.misses << "\r\n";
    tmp_stream << "hits_per_sec:" << cache_info.hits_per_sec << "\r\n";
    tmp_stream << "read_cmd_per_sec:" << cache_info.read_cmd_per_sec << "\r\n";
    tmp_stream << "hitratio_per_sec:" << std::setprecision(4) << cache_info.hitratio_per_sec << "%" <<"\r\n";
    tmp_stream << "hitratio_all:" << std::setprecision(4) << cache_info.hitratio_all << "%" <<"\r\n";
    tmp_stream << "load_keys_per_sec:" << cache_info.load_keys_per_sec << "\r\n";
    tmp_stream << "waitting_load_keys_num:" << cache_info.waitting_load_keys_num << "\r\n";

    info.append(tmp_stream.str());
}

void ConfigCmd::DoInitial(PikaCmdArgsType &argv, const CmdInfo* const ptr_info) {
    if (!ptr_info->CheckArg(argv.size())) {
        res_.SetRes(CmdRes::kWrongNum, kCmdNameConfig);
        return;
    }
    size_t argc = argv.size();
    slash::StringToLower(argv[1]);
    if (argv[1] == "get") {
        if (argc != 3) {
            res_.SetRes(CmdRes::kErrOther, "Wrong number of arguments for CONFIG get");
            return;
        }
    } else if (argv[1] == "set") {
        if (argc == 3 && argv[2] != "*") {
            res_.SetRes(CmdRes::kErrOther, "Wrong number of arguments for CONFIG set");
            return;
        } else if (argc != 4 && argc != 3) {
            res_.SetRes(CmdRes::kErrOther, "Wrong number of arguments for CONFIG set");
            return;
        }
    } else if (argv[1] == "rewrite") {
        if (argc != 2) {
            res_.SetRes(CmdRes::kErrOther, "Wrong number of arguments for CONFIG rewrite");
            return;
        }
    } else if (argv[1] == "resetstat") {
        if (argc != 2) {
            res_.SetRes(CmdRes::kErrOther, "Wrong number of arguments for CONFIG resetstat");
            return;
        }
    } else {
        res_.SetRes(CmdRes::kErrOther, "CONFIG subcommand must be one of GET, SET, RESETSTAT, REWRITE");
        return;
    }
    config_args_v_.assign(argv.begin()+1, argv.end());
    return;
}

void ConfigCmd::Do() {
    std::string config_ret;
    if (config_args_v_[0] == "get") {
        ConfigGet(config_ret);
    } else if (config_args_v_[0] == "set") {
        ConfigSet(config_ret);
    } else if (config_args_v_[0] == "rewrite") {
        ConfigRewrite(config_ret);
    } else if (config_args_v_[0] == "resetstat") {
        ConfigResetstat(config_ret);
    }
    res_.AppendStringRaw(config_ret);
    return;
}

static void EncodeString(std::string *dst, const std::string &value) {
    dst->append("$");
    dst->append(std::to_string(value.size()));
    dst->append("\r\n");
    dst->append(value.data(), value.size());
    dst->append("\r\n");
}

static void EncodeInt32(std::string *dst, const int32_t v) {
    std::string vstr = std::to_string(v);
    dst->append("$");
    dst->append(std::to_string(vstr.length()));
    dst->append("\r\n");
    dst->append(vstr);
    dst->append("\r\n");
}

static void EncodeInt64(std::string *dst, const int64_t v) {
    std::string vstr = std::to_string(v);
    dst->append("$");
    dst->append(std::to_string(vstr.length()));
    dst->append("\r\n");
    dst->append(vstr);
    dst->append("\r\n");
}

void ConfigCmd::ConfigGet(std::string &ret) {
    std::string get_item = config_args_v_[1];
    if (get_item == "port") {
        ret = "*2\r\n";
        EncodeString(&ret, "port");
        EncodeInt32(&ret, g_pika_conf->port());
    } else if (get_item == "thread-num") {
        ret = "*2\r\n";
        EncodeString(&ret, "thread-num");
        EncodeInt32(&ret, g_pika_conf->thread_num());
    } else if (get_item == "sync-thread-num") {
        ret = "*2\r\n";
        EncodeString(&ret, "sync-thread-num");
        EncodeInt32(&ret, g_pika_conf->sync_thread_num());
    } else if (get_item == "sync-buffer-size") {
        ret = "*2\r\n";
        EncodeString(&ret, "sync-buffer-size");
        EncodeInt32(&ret, g_pika_conf->sync_buffer_size());
    } else if (get_item == "log-path") {
        ret = "*2\r\n";
        EncodeString(&ret, "log-path");
        EncodeString(&ret, g_pika_conf->log_path());
    } else if (get_item == "loglevel") {
        ret = "*2\r\n";
        EncodeString(&ret, "loglevel");
        EncodeString(&ret, g_pika_conf->log_level() ? "ERROR" : "INFO");
    } else if (get_item == "max-log-size") {
        ret = "*2\r\n";
        EncodeString(&ret, "max-log-size");
        EncodeInt32(&ret, g_pika_conf->max_log_size());
    } else if (get_item == "db-path") {
        ret = "*2\r\n";
        EncodeString(&ret, "db-path");
        EncodeString(&ret, g_pika_conf->db_path());
    } else if (get_item == "db-sync-path") {
        ret = "*2\r\n";
        EncodeString(&ret, "db-sync-path");
        EncodeString(&ret, g_pika_conf->db_sync_path());
    } else if (get_item == "db-sync-speed") {
        ret = "*2\r\n";
        EncodeString(&ret, "db-sync-speed");
        EncodeInt32(&ret, g_pika_conf->db_sync_speed());
    } else if (get_item == "compact-cron") {
        ret = "*2\r\n";
        EncodeString(&ret, "compact-cron");
        EncodeString(&ret, g_pika_conf->compact_cron());
    } else if (get_item == "compact-interval") {
        ret = "*2\r\n";
        EncodeString(&ret, "compact-interval");
        EncodeString(&ret, g_pika_conf->compact_interval());
    } else if (get_item == "maxmemory") {
        ret = "*2\r\n";
        EncodeString(&ret, "maxmemory");
        EncodeInt64(&ret, g_pika_server->db_size_);
    } else if (get_item == "write-buffer-size") {
        ret = "*2\r\n";
        EncodeString(&ret, "write-buffer-size");
        EncodeInt64(&ret, g_pika_conf->write_buffer_size());
    }  else if (get_item == "max-write-buffer-number") {
        ret = "*2\r\n";
        EncodeString(&ret, "max-write-buffer-number");
        EncodeInt32(&ret, g_pika_conf->max_write_buffer_number());
    } else if (get_item == "timeout") {
        ret = "*2\r\n";
        EncodeString(&ret, "timeout");
        EncodeInt32(&ret, g_pika_conf->timeout());
    } else if(get_item == "fresh-info-interval"){
        ret = "*2\r\n";
        EncodeString(&ret, "fresh-info-interval");
        EncodeInt32(&ret, g_pika_conf->fresh_info_interval());
    } else if (get_item == "requirepass") {
        ret = "*2\r\n";
        EncodeString(&ret, "requirepass");
        EncodeString(&ret, g_pika_conf->requirepass());
    }  else if (get_item == "masterauth") {
        ret = "*2\r\n";
        EncodeString(&ret, "masterauth");
        EncodeString(&ret, g_pika_conf->masterauth());
    } else if (get_item == "userpass") {
        ret = "*2\r\n";
        EncodeString(&ret, "userpass");
        EncodeString(&ret, g_pika_conf->userpass());
    } else if (get_item == "userblacklist") {
        ret = "*2\r\n";
        EncodeString(&ret, "userblacklist");
        EncodeString(&ret, (g_pika_conf->suser_blacklist()).c_str());
    } else if (get_item == "dump-prefix") {
        ret = "*2\r\n";
        EncodeString(&ret, "dump-prefix");
        EncodeString(&ret, g_pika_conf->bgsave_prefix());
    } else if (get_item == "daemonize") {
        ret = "*2\r\n";
        EncodeString(&ret, "daemonize");
        EncodeString(&ret, g_pika_conf->daemonize() ? "yes" : "no");
    } else if (get_item == "slotmigrate") {
        ret = "*2\r\n";
        EncodeString(&ret, "slotmigrate");
        EncodeString(&ret, g_pika_conf->slotmigrate() ? "yes" : "no");
    } else if (get_item == "slotmigrate-thread-num") {
        ret = "*2\r\n";
        EncodeString(&ret, "slotmigrate-thread-num");
        EncodeInt32(&ret, g_pika_conf->slotmigrate_thread_num());
    } else if (get_item == "thread-migrate-keys-num") {
        ret = "*2\r\n";
        EncodeString(&ret, "thread-migrate-keys-num");
        EncodeInt32(&ret, g_pika_conf->thread_migrate_keys_num());
    } else if (get_item == "dump-path") {
        ret = "*2\r\n";
        EncodeString(&ret, "dump-path");
        EncodeString(&ret, g_pika_conf->bgsave_path());
    } else if (get_item == "dump-expire") {
        ret = "*2\r\n";
        EncodeString(&ret, "dump-expire");
        EncodeInt32(&ret, g_pika_conf->expire_dump_days());
    } else if (get_item == "pidfile") {
        ret = "*2\r\n";
        EncodeString(&ret, "pidfile");
        EncodeString(&ret, g_pika_conf->pidfile());
    } else if (get_item == "maxclients") {
        ret = "*2\r\n";
        EncodeString(&ret, "maxclients");
        EncodeInt32(&ret, g_pika_conf->maxclients());
    } else if (get_item == "target-file-size-base") {
        ret = "*2\r\n";
        EncodeString(&ret, "target-file-size-base");
        EncodeInt32(&ret, g_pika_conf->target_file_size_base());
    } else if (get_item == "max-bytes-for-level-base") {
        ret = "*2\r\n";
        EncodeString(&ret, "max-bytes-for-level-base");
        EncodeInt32(&ret, g_pika_conf->max_bytes_for_level_base());
    } else if (get_item == "max-background-flushes") {
        ret = "*2\r\n";
        EncodeString(&ret, "max-background-flushes");
        EncodeInt32(&ret, g_pika_conf->max_background_flushes());
    } else if (get_item == "max-background-compactions") {
        ret = "*2\r\n";
        EncodeString(&ret, "max-background-compactions");
        EncodeInt32(&ret, g_pika_conf->max_background_compactions());
    } else if (get_item == "max-cache-files") {
        ret = "*2\r\n";
        EncodeString(&ret, "max-cache-files");
        EncodeInt32(&ret, g_pika_conf->max_cache_files());
    } else if (get_item == "max-bytes-for-level-multiplier") {
        ret = "*2\r\n";
        EncodeString(&ret, "max-bytes-for-level-multiplier");
        EncodeInt32(&ret, g_pika_conf->max_bytes_for_level_multiplier());
    } else if (get_item == "disable-auto-compactions") {
        ret = "*2\r\n";
        EncodeString(&ret, "disable-auto-compactions");
        EncodeInt32(&ret, g_pika_conf->disable_auto_compactions());
    } else if (get_item == "block-size") {
        ret = "*2\r\n";
        EncodeString(&ret, "block-size");
        EncodeInt32(&ret, g_pika_conf->block_size());
    } else if (get_item == "block-cache") {
        ret = "*2\r\n";
        EncodeString(&ret, "block-cache");
        EncodeInt32(&ret, g_pika_conf->block_cache());
    } else if (get_item == "share-block-cache") {
        ret = "*2\r\n";
        EncodeString(&ret, "share-block-cache");
        EncodeString(&ret, g_pika_conf->share_block_cache() ? "yes" : "no");
    } else if (get_item == "cache-index-and-filter-blocks") {
        ret = "*2\r\n";
        EncodeString(&ret, "cache-index-and-filter-blocks");
        EncodeString(&ret, g_pika_conf->cache_index_and_filter_blocks() ? "yes" : "no");
    } else if (get_item == "optimize-filters-for-hits") {
        ret = "*2\r\n";
        EncodeString(&ret, "optimize-filters-for-hits");
        EncodeString(&ret, g_pika_conf->optimize_filters_for_hits() ? "yes" : "no"); 
    } else if (get_item == "level-compaction-dynamic-level-bytes") {
        ret = "*2\r\n";
        EncodeString(&ret, "level-compaction-dynamic-level-bytes");
        EncodeString(&ret, g_pika_conf->level_compaction_dynamic_level_bytes() ? "yes" : "no");
    } else if (get_item == "max-subcompactions") {
        ret = "*2\r\n";
        EncodeString(&ret, "max-subcompactions");
        EncodeInt32(&ret, g_pika_conf->max_subcompactions());
    } else if (get_item == "expire-logs-days") {
        ret = "*2\r\n";
        EncodeString(&ret, "expire-logs-days");
        EncodeInt32(&ret, g_pika_conf->expire_logs_days());
    } else if (get_item == "expire-logs-nums") {
        ret = "*2\r\n";
        EncodeString(&ret, "expire-logs-nums");
        EncodeInt32(&ret, g_pika_conf->expire_logs_nums());
    } else if (get_item == "binlog-writer-queue-size") {
        ret = "*2\r\n";
        EncodeString(&ret, "binlog-writer-queue-size");
        EncodeInt32(&ret, g_pika_conf->binlog_writer_queue_size());
    } else if (get_item == "binlog-writer-method") {
        ret = "*2\r\n";
        EncodeString(&ret, "binlog-writer-method");
        EncodeString(&ret, g_pika_conf->binlog_writer_method());
    } else if (get_item == "binlog-writer-num") {
        ret = "*2\r\n";
        EncodeString(&ret, "binlog-writer-num");
        EncodeInt32(&ret, g_pika_conf->binlog_writer_num());
    } else if (get_item == "root-connection-num" ) {
        ret = "*2\r\n";
        EncodeString(&ret, "root-connection-num");
        EncodeInt32(&ret, g_pika_conf->root_connection_num());
    } else if (get_item == "slowlog-log-slower-than") {
        ret = "*2\r\n";
        EncodeString(&ret, "slowlog-log-slower-than");
        EncodeInt32(&ret, g_pika_conf->slowlog_slower_than());
    } else if (get_item == "slowlog-max-len") {
        ret = "*2\r\n";
        EncodeString(&ret, "slowlog-max-len");
        EncodeInt32(&ret, g_pika_conf->slowlog_max_len());
    } else if (get_item == "binlog-file-size") {
        ret = "*2\r\n";
        EncodeString(&ret, "binlog-file-size");
        EncodeInt32(&ret, g_pika_conf->binlog_file_size());
    } else if (get_item == "compression") {
        ret = "*2\r\n";
        EncodeString(&ret, "compression");
        EncodeString(&ret, g_pika_conf->compression());
    } else if (get_item == "slave-read-only") {
        ret = "*2\r\n";
        EncodeString(&ret, "slave-read-only");
        if (g_pika_conf->readonly()) {
            EncodeString(&ret, "yes");
        } else {
            EncodeString(&ret, "no");
        }
    } else if (get_item == "slaveof") {
        ret = "*2\r\n";
        EncodeString(&ret, "slaveof");
        EncodeString(&ret, g_pika_conf->slaveof());
    } else if (get_item == "level0-file-num-compaction-trigger") {
        ret = "*2\r\n";
        EncodeString(&ret, "level0-file-num-compaction-trigger");
        EncodeInt32(&ret, g_pika_conf->level0_file_num_compaction_trigger());
    } else if (get_item == "level0-stop-writes-trigger") {
        ret = "*2\r\n";
        EncodeString(&ret, "level0-stop-writes-trigger");
        EncodeInt32(&ret, g_pika_conf->level0_stop_writes_trigger());
    } else if (get_item == "level0-slowdown-writes-trigger") {
        ret = "*2\r\n";
        EncodeString(&ret, "level0-slowdown-writes-trigger");
        EncodeInt32(&ret, g_pika_conf->level0_slowdown_writes_trigger());
    } else if (get_item == "slave-priority") {
        ret = "*2\r\n";
        EncodeString(&ret, "slave-priority");
        EncodeInt32(&ret, g_pika_conf->slave_priority());
    } else if (get_item == "cache-num") {
        ret = "*2\r\n";
        EncodeString(&ret, "cache-num");
        EncodeInt32(&ret, g_pika_conf->cache_num());
    } else if (get_item == "cache-model") {
        ret = "*2\r\n";
        EncodeString(&ret, "cache-model");
        EncodeInt32(&ret, g_pika_conf->cache_model());
    } else if (get_item == "cache-maxmemory") {
        ret = "*2\r\n";
        EncodeString(&ret, "cache-maxmemory");
        EncodeInt64(&ret, g_pika_conf->cache_maxmemory());
    } else if (get_item == "cache-maxmemory-policy") {
        ret = "*2\r\n";
        EncodeString(&ret, "cache-maxmemory-policy");
        EncodeInt32(&ret, g_pika_conf->cache_maxmemory_policy());
    } else if (get_item == "cache-maxmemory-samples") {
        ret = "*2\r\n";
        EncodeString(&ret, "cache-maxmemory-samples");
        EncodeInt32(&ret, g_pika_conf->cache_maxmemory_samples());
    } else if (get_item == "cache-lfu-decay-time") {
        ret = "*2\r\n";
        EncodeString(&ret, "cache-lfu-decay-time");
        EncodeInt32(&ret, g_pika_conf->cache_lfu_decay_time());
    } else if (get_item == "*") {
        ret = "*134\r\n";
        EncodeString(&ret, "port");
        EncodeInt32(&ret, g_pika_conf->port());
        EncodeString(&ret, "thread-num");
        EncodeInt32(&ret, g_pika_conf->thread_num());
        EncodeString(&ret, "sync-thread-num");
        EncodeInt32(&ret, g_pika_conf->sync_thread_num());
        EncodeString(&ret, "sync-buffer-size");
        EncodeInt32(&ret, g_pika_conf->sync_buffer_size());
        EncodeString(&ret, "log-path");
        EncodeString(&ret, g_pika_conf->log_path());
        EncodeString(&ret, "loglevel");
        EncodeString(&ret, g_pika_conf->log_level() ? "ERROR" : "INFO");
        EncodeString(&ret, "max-log-size");
        EncodeInt32(&ret, g_pika_conf->max_log_size());
        EncodeString(&ret, "db-path");
        EncodeString(&ret, g_pika_conf->db_path());
        EncodeString(&ret, "maxmemory");
        EncodeInt64(&ret, g_pika_server->db_size_);
        EncodeString(&ret, "write-buffer-size");
        EncodeInt64(&ret, g_pika_conf->write_buffer_size());
        EncodeString(&ret, "max-write-buffer-number");
        EncodeInt32(&ret, g_pika_conf->max_write_buffer_number());
        EncodeString(&ret, "timeout");
        EncodeInt32(&ret, g_pika_conf->timeout());
        EncodeString(&ret, "fresh-info-interval");
        EncodeInt32(&ret, g_pika_conf->fresh_info_interval());
        EncodeString(&ret, "requirepass");
        EncodeString(&ret, g_pika_conf->requirepass());
        EncodeString(&ret, "masterauth");
        EncodeString(&ret, g_pika_conf->masterauth());
        EncodeString(&ret, "userpass");
        EncodeString(&ret, g_pika_conf->userpass());
        EncodeString(&ret, "userblacklist");
        EncodeString(&ret, g_pika_conf->suser_blacklist());
        EncodeString(&ret, "daemonize");
        EncodeInt32(&ret, g_pika_conf->daemonize());
        EncodeString(&ret, "slotmigrate");
        EncodeInt32(&ret, g_pika_conf->slotmigrate());
        EncodeString(&ret, "slotmigrate-thread-num");
        EncodeInt32(&ret, g_pika_conf->slotmigrate_thread_num());
        EncodeString(&ret, "thread-migrate-keys-num");
        EncodeInt32(&ret, g_pika_conf->thread_migrate_keys_num());
        EncodeString(&ret, "dump-path");
        EncodeString(&ret, g_pika_conf->bgsave_path());
        EncodeString(&ret, "dump-expire");
        EncodeInt32(&ret, g_pika_conf->expire_dump_days());
        EncodeString(&ret, "dump-prefix");
        EncodeString(&ret, g_pika_conf->bgsave_prefix());
        EncodeString(&ret, "pidfile");
        EncodeString(&ret, g_pika_conf->pidfile());
        EncodeString(&ret, "maxclients");
        EncodeInt32(&ret, g_pika_conf->maxclients());
        EncodeString(&ret, "target-file-size-base");
        EncodeInt32(&ret, g_pika_conf->target_file_size_base());
        EncodeString(&ret, "max-bytes-for-level-base");
        EncodeInt32(&ret, g_pika_conf->max_bytes_for_level_base());
        EncodeString(&ret, "max-background-flushes");
        EncodeInt32(&ret, g_pika_conf->max_background_flushes());
        EncodeString(&ret, "max-background-compactions");
        EncodeInt32(&ret, g_pika_conf->max_background_compactions());
        EncodeString(&ret, "max-cache-files");
        EncodeInt32(&ret, g_pika_conf->max_cache_files());
        EncodeString(&ret, "max-bytes-for-level-multiplier");
        EncodeInt32(&ret, g_pika_conf->max_bytes_for_level_multiplier());
        EncodeString(&ret, "disable-auto-compactions");
        EncodeInt32(&ret, g_pika_conf->disable_auto_compactions());
        EncodeString(&ret, "block-size");
        EncodeInt32(&ret, g_pika_conf->block_size());
        EncodeString(&ret, "block-cache");
        EncodeInt32(&ret, g_pika_conf->block_cache());
        EncodeString(&ret, "share-block-cache");
        EncodeString(&ret, g_pika_conf->share_block_cache() ? "yes" : "no");
        EncodeString(&ret, "cache-index-and-filter-blocks");
        EncodeString(&ret, g_pika_conf->cache_index_and_filter_blocks() ? "yes" : "no");
        EncodeString(&ret, "optimize-filters-for-hits");
        EncodeString(&ret, g_pika_conf->optimize_filters_for_hits() ? "yes" : "no"); 
        EncodeString(&ret, "level-compaction-dynamic-level-bytes");
        EncodeString(&ret, g_pika_conf->level_compaction_dynamic_level_bytes() ? "yes" : "no");
        EncodeString(&ret, "max-subcompactions");
        EncodeInt32(&ret, g_pika_conf->max_subcompactions());
        EncodeString(&ret, "expire-logs-days");
        EncodeInt32(&ret, g_pika_conf->expire_logs_days());
        EncodeString(&ret, "expire-logs-nums");
        EncodeInt32(&ret, g_pika_conf->expire_logs_nums());
        EncodeString(&ret, "binlog-writer-queue-size");
        EncodeInt32(&ret, g_pika_conf->binlog_writer_queue_size());
        EncodeString(&ret, "binlog-writer-method");
        EncodeString(&ret, g_pika_conf->binlog_writer_method());
        EncodeString(&ret, "binlog-writer-num");
        EncodeInt32(&ret, g_pika_conf->binlog_writer_num());
        EncodeString(&ret, "root-connection-num");
        EncodeInt32(&ret, g_pika_conf->root_connection_num());
        EncodeString(&ret, "slowlog-log-slower-than");
        EncodeInt32(&ret, g_pika_conf->slowlog_slower_than());
        EncodeString(&ret, "slowlog-max-len");
        EncodeInt32(&ret, g_pika_conf->slowlog_max_len());
        EncodeString(&ret, "slave-read-only");
        EncodeInt32(&ret, g_pika_conf->readonly());
        EncodeString(&ret, "binlog-file-size");
        EncodeInt32(&ret, g_pika_conf->binlog_file_size());
        EncodeString(&ret, "compression");
        EncodeString(&ret, g_pika_conf->compression());
        EncodeString(&ret, "db-sync-path");
        EncodeString(&ret, g_pika_conf->db_sync_path());
        EncodeString(&ret, "db-sync-speed");
        EncodeInt32(&ret, g_pika_conf->db_sync_speed());
        EncodeString(&ret, "compact-cron");
        EncodeString(&ret, g_pika_conf->compact_cron());
        EncodeString(&ret, "compact-interval");
        EncodeString(&ret, g_pika_conf->compact_interval());
        EncodeString(&ret, "network-interface");
        EncodeString(&ret, g_pika_conf->network_interface());
        EncodeString(&ret, "slaveof");
        EncodeString(&ret, g_pika_conf->slaveof());
        EncodeString(&ret, "level0-file-num-compaction-trigger");
        EncodeInt32(&ret, g_pika_conf->level0_file_num_compaction_trigger());
        EncodeString(&ret, "level0-slowdown-writes-trigger");
        EncodeInt32(&ret, g_pika_conf->level0_slowdown_writes_trigger());
        EncodeString(&ret, "level0-stop-writes-trigger");
        EncodeInt32(&ret, g_pika_conf->level0_stop_writes_trigger());
        EncodeString(&ret, "slave-priority");
        EncodeInt32(&ret, g_pika_conf->slave_priority());
        EncodeString(&ret, "cache-num");
        EncodeInt32(&ret, g_pika_conf->cache_num());
        EncodeString(&ret, "cache-model");
        EncodeInt32(&ret, g_pika_conf->cache_model());
        EncodeString(&ret, "cache-maxmemory");
        EncodeInt64(&ret, g_pika_conf->cache_maxmemory());
        EncodeString(&ret, "cache-maxmemory-policy");
        EncodeInt32(&ret, g_pika_conf->cache_maxmemory_policy());
        EncodeString(&ret, "cache-maxmemory-samples");
        EncodeInt32(&ret, g_pika_conf->cache_maxmemory_samples());
        EncodeString(&ret, "cache-lfu-decay-time");
        EncodeInt32(&ret, g_pika_conf->cache_lfu_decay_time());
    } else {
        ret = "*0\r\n";
    }
}

void ConfigCmd::ConfigSet(std::string& ret) {
    std::string set_item = config_args_v_[1];
    if (set_item == "*") {
        ret = "*36\r\n";
        EncodeString(&ret, "loglevel");
        EncodeString(&ret, "max-log-size");
        EncodeString(&ret, "timeout");
        EncodeString(&ret, "fresh-info-interval");
        EncodeString(&ret, "requirepass");
        EncodeString(&ret, "masterauth");
        EncodeString(&ret, "slotmigrate");
        EncodeString(&ret, "userpass");
        EncodeString(&ret, "userblacklist");
        EncodeString(&ret, "dump-prefix");
        EncodeString(&ret, "maxclients");
        EncodeString(&ret, "dump-expire");
        EncodeString(&ret, "expire-logs-days");
        EncodeString(&ret, "expire-logs-nums");
        EncodeString(&ret, "binlog-writer-queue-size");
        EncodeString(&ret, "root-connection-num");
        EncodeString(&ret, "slowlog-log-slower-than");
        EncodeString(&ret, "slave-read-only");
        EncodeString(&ret, "db-sync-speed");
        EncodeString(&ret, "compact-cron");
        EncodeString(&ret, "compact-interval");
        EncodeString(&ret, "write-buffer-size");
        EncodeString(&ret, "target-file-size-base");
        EncodeString(&ret, "max-bytes-for-level-base");
        EncodeString(&ret, "max-write-buffer-number");
        EncodeString(&ret, "disable-auto-compactions");
        EncodeString(&ret, "level0-file-num-compaction-trigger");
        EncodeString(&ret, "level0-slowdown-writes-trigger");
        EncodeString(&ret, "level0-stop-writes-trigger");
        EncodeString(&ret, "slave-priority");
        EncodeString(&ret, "cache-num");
        EncodeString(&ret, "cache-model");
        EncodeString(&ret, "cache-maxmemory");
        EncodeString(&ret, "cache-maxmemory-policy");
        EncodeString(&ret, "cache-maxmemory-samples");
        EncodeString(&ret, "cache-lfu-decay-time");
        return;
    }
    std::string value = config_args_v_[2];
    long int ival;
    if (set_item == "loglevel") {
        slash::StringToLower(value);
        if (value == "info") {
            ival = 0;
        } else if (value == "error") {
            ival = 1;
        } else {
            ret = "-ERR Invalid argument " + value + " for CONFIG SET 'loglevel'\r\n";
            return;
        }
        g_pika_conf->SetLogLevel(ival);
        FLAGS_minloglevel = g_pika_conf->log_level();
        ret = "+OK\r\n";
    } else if (set_item == "max-log-size") {
        if (!slash::string2l(value.data(), value.size(), &ival) || ival <= 0 ) {
            ret = "-ERR Invalid argument " + value + " for CONFIG SET 'max-log-size'\r\n";
            return;
        }
        g_pika_conf->SetMaxLogSize(ival);
        FLAGS_max_log_size = ival;
        ret = "+OK\r\n";
    } else if (set_item == "timeout") {
        if (!slash::string2l(value.data(), value.size(), &ival)) {
            ret = "-ERR Invalid argument " + value + " for CONFIG SET 'timeout'\r\n";
            return;
        }
        g_pika_conf->SetTimeout(ival);
        ret = "+OK\r\n";
    } else if(set_item == "fresh-info-interval"){
        if (!slash::string2l(value.data(), value.size(), &ival) || ival <= 0) {
            ret = "-ERR Invalid argument " + value + " for CONFIG SET 'fresh-info-interval'\r\n";
            return;
        }
		g_pika_conf->SetFreshInfoInterval(ival);
		ret = "+OK\r\n";
    } else if (set_item == "requirepass") {
        g_pika_conf->SetRequirePass(value);
        ret = "+OK\r\n";
    } else if (set_item == "masterauth") {
        g_pika_conf->SetMasterAuth(value);
        ret = "+OK\r\n";
    } else if (set_item == "slotmigrate") {
        slash::StringToLower(value);
        bool is_migrate;
        if (value == "1" || value == "yes") {
            is_migrate = true;
        } else if (value == "0" || value == "no") {
            is_migrate = false;
        } else {
            ret = "-ERR Invalid argument " + value + " for CONFIG SET 'slotmigrate'\r\n";
            return;
        }
        g_pika_conf->SetSlotMigrate(is_migrate);
        ret = "+OK\r\n";
    } else if (set_item == "slotmigrate-thread-num") {
        if (!slash::string2l(value.data(), value.size(), &ival)) {
            ret = "-ERR Invalid argument " + value + " for CONFIG SET 'slotmigrate-thread-num'\r\n";
            return;
        }
        long int migrate_thread_num = (1 > ival || 24 < ival) ? 8 : ival;
        g_pika_conf->SetSlotMigrateThreadNum(migrate_thread_num);
        ret = "+OK\r\n";
    } else if (set_item == "thread-migrate-keys-num") {
        if (!slash::string2l(value.data(), value.size(), &ival)) {
            ret = "-ERR Invalid argument " + value + " for CONFIG SET 'thread-migrate-keys-num'\r\n";
            return;
        }
        long int thread_migrate_keys_num = (8 > ival || 128 < ival) ? 64 : ival;
        g_pika_conf->SetThreadMigrateKeysNum(thread_migrate_keys_num);
        ret = "+OK\r\n";
    } else if (set_item == "userpass") {
        g_pika_conf->SetUserPass(value);
        ret = "+OK\r\n";
    } else if (set_item == "userblacklist") {
        g_pika_conf->SetUserBlackList(value);
        ret = "+OK\r\n";
    } else if (set_item == "dump-prefix") {
        g_pika_conf->SetBgsavePrefix(value);
        ret = "+OK\r\n";
    } else if (set_item == "maxclients") {
        if (!slash::string2l(value.data(), value.size(), &ival) || ival <= 0) {
            ret = "-ERR Invalid argument " + value + " for CONFIG SET 'maxclients'\r\n";
            return;
        }
        g_pika_conf->SetMaxConnection(ival);
        ret = "+OK\r\n";
    } else if (set_item == "dump-expire") {
        if (!slash::string2l(value.data(), value.size(), &ival)) {
            ret = "-ERR Invalid argument " + value + " for CONFIG SET 'dump-expire'\r\n";
            return;
        }
        g_pika_conf->SetExpireDumpDays(ival);
        ret = "+OK\r\n";
    } else if (set_item == "expire-logs-days") {
        if (!slash::string2l(value.data(), value.size(), &ival) || ival <= 0) {
            ret = "-ERR Invalid argument " + value + " for CONFIG SET 'expire-logs-days'\r\n";
            return;
        }
        g_pika_conf->SetExpireLogsDays(ival);
        ret = "+OK\r\n";
    } else if (set_item == "expire-logs-nums") {
        if (!slash::string2l(value.data(), value.size(), &ival) || ival <= 0) {
            ret = "-ERR Invalid argument " + value + " for CONFIG SET 'expire-logs-nums'\r\n";
            return;
        }
        g_pika_conf->SetExpireLogsNums(ival);
        ret = "+OK\r\n";
    } else if (set_item == "binlog-writer-queue-size") {
        if (!slash::string2l(value.data(), value.size(), &ival) || ival <= 0 || ival > 10000) {
            ret = "-ERR Invalid argument " + value + " for CONFIG SET 'binlog-writer-queue-size'\r\n";
            return;
        }
        g_pika_conf->SetBinlogWriterQueueSize(ival);
        for (int i=0; i<g_pika_conf->binlog_writer_num(); i++) {
            g_pika_server->binlog_write_thread_[i]->SetMaxCmdsQueueSize(ival);
        }
        ret = "+OK\r\n";
    } else if (set_item == "root-connection-num") {
        if (!slash::string2l(value.data(), value.size(), &ival) || ival <= 0) {
            ret = "-ERR Invalid argument " + value + " for CONFIG SET 'root-connection-num'\r\n";
            return;
        }
        g_pika_conf->SetRootConnectionNum(ival);
        ret = "+OK\r\n";
    } else if (set_item == "slowlog-log-slower-than") {
        if (!slash::string2l(value.data(), value.size(), &ival) || ival <= 0) {
            ret = "-ERR Invalid argument " + value + " for CONFIG SET 'slowlog-log-slower-than'\r\n";
            return;
        }
        g_pika_conf->SetSlowlogSlowerThan(ival);
        ret = "+OK\r\n";
    } else if (set_item == "slowlog-max-len") {
        if (!slash::string2l(value.data(), value.size(), &ival)) {
            ret = "-ERR Invalid argument " + value + " for CONFIG SET 'slowlog-max-len'\r\n";
            return;
        }
        long int tmp_val = (100 > ival || 10000000 < ival) ? 12800 : ival;
        g_pika_conf->SetSlowlogMaxLen(tmp_val);
        g_pika_server->SlowlogTrim();
        ret = "+OK\r\n";
    } else if (set_item == "slave-read-only") {
        slash::StringToLower(value);
        bool is_readonly;
        if (value == "1" || value == "yes") {
            is_readonly = true;
        } else if (value == "0" || value == "no") {
            is_readonly = false;
        } else {
            ret = "-ERR Invalid argument " + value + " for CONFIG SET 'slave-read-only'\r\n";
            return;
        }
        g_pika_conf->SetReadonly(is_readonly);
        ret = "+OK\r\n";
    } else if (set_item == "db-sync-speed") {
        if (!slash::string2l(value.data(), value.size(), &ival)) {
            ret = "-ERR Invalid argument " + value + " for CONFIG SET 'db-sync-speed(MB)'\r\n";
            return;
        }
        if (ival < 0 || ival > 125) {
            ival = 125;
        }
        g_pika_conf->SetDbSyncSpeed(ival);
        ret = "+OK\r\n";
    } else if (set_item == "compact-cron") {
        bool invalid = false;
        if (value != "") {
            std::string::size_type len = value.length();
            std::string::size_type colon = value.find("-");
            std::string::size_type underline = value.find("/");
            if (colon == std::string::npos || underline == std::string::npos ||
                    colon >= underline || colon + 1 >= len ||
                    colon + 1 == underline || underline + 1 >= len) {
                invalid = true;
            } else {
                int start = std::atoi(value.substr(0, colon).c_str());
                int end = std::atoi(value.substr(colon+1, underline).c_str());
                int usage = std::atoi(value.substr(underline+1).c_str());
                if (start < 0 || start > 23 || end < 0 || end > 23 || usage < 0 || usage > 100) {
                    invalid = true;
                }
            }
        }
        if (invalid) {
            ret = "-ERR invalid compact-cron\r\n";
            return;
        } else {
            g_pika_conf->SetCompactCron(value);
            ret = "+OK\r\n";
        }
    } else if (set_item == "compact-interval") {
        bool invalid = false;
        if (value != "") {
            std::string::size_type len = value.length();
            std::string::size_type slash = value.find("/");
            if (slash == std::string::npos || slash + 1 >= len) {
                invalid = true;
            } else {
                int interval = std::atoi(value.substr(0, slash).c_str());
                int usage = std::atoi(value.substr(slash+1).c_str());
                if (interval <= 0 || usage < 0 || usage > 100) {
                    invalid = true;
                }
            }
        }
        if (invalid) {
            ret = "-ERR invalid compact-interval\r\n";
            return;
        } else {
            g_pika_conf->SetCompactInterval(value);
            ret = "+OK\r\n";
        }
    } else if (set_item == "write-buffer-size") {
        long long ival = 0;
        if (!slash::string2ll(value.data(), value.size(), &ival) || ival <= 0) {
            ret = "-ERR Invalid argument " + value + " for CONFIG SET 'write-buffer-size'\r\n";
            return;
        }
        g_pika_conf->SetWriteBufferSize(ival);
        rocksdb::Status s = g_pika_server->db()->ResetOption("write_buffer_size", slash::StringToLower(value));
        if (!s.ok()) {
            ret = "-ERR Reset rocksdb 'write-buffer-size' failed\r\n";
            return;
        }
        ret = "+OK\r\n";
    } else if (set_item == "max-write-buffer-number") {
        if (!slash::string2l(value.data(), value.size(), &ival) || ival < 2) {
            ret = "-ERR Invalid argument " + value + " for CONFIG SET 'max-write-buffer-number'\r\n";
            return;
        }
        g_pika_conf->SetMaxWriteBufferNumber(ival);
        rocksdb::Status s = g_pika_server->db()->ResetOption("max_write_buffer_number", slash::StringToLower(value));
        if (!s.ok()) {
            ret = "-ERR Reset rocksdb 'max-write-buffer-number' failed\r\n";
            return;
        }
        ret = "+OK\r\n";
    } else if (set_item == "target-file-size-base") {
        if (!slash::string2l(value.data(), value.size(), &ival) || ival <= 0) {
            ret = "-ERR Invalid argument " + value + " for CONFIG SET 'target-file-size-base'\r\n";
            return;
        }
        g_pika_conf->SetTargetFileSizeBase(ival);
        rocksdb::Status s = g_pika_server->db()->ResetOption("target_file_size_base", slash::StringToLower(value));
        if (!s.ok()) {
            ret = "-ERR Reset rocksdb 'target-file-size-base' failed\r\n";
            return;
        }
        ret = "+OK\r\n";
    }  else if (set_item == "max-bytes-for-level-base") {
        if (!slash::string2l(value.data(), value.size(), &ival) || ival <= 0) {
            ret = "-ERR Invalid argument " + value + " for CONFIG SET 'max-bytes-for-level-base'\r\n";
            return;
        }
        g_pika_conf->SetMaxBytesForLevelBase(ival);
        rocksdb::Status s = g_pika_server->db()->ResetOption("max_bytes_for_level_base", slash::StringToLower(value));
        if (!s.ok()) {
            ret = "-ERR Reset rocksdb 'max-bytes-for-level-base' failed\r\n";
            return;
        }
        ret = "+OK\r\n";
    } else if (set_item == "disable-auto-compactions") {
        slash::StringToLower(value);
        bool disable_auto_compactions;
        if (value == "1" || value == "yes") {
            disable_auto_compactions = true;
        } else if (value == "0" || value == "no") {
            disable_auto_compactions = false;
        } else {
            ret = "-ERR Invalid argument " + value + " for CONFIG SET 'disable-auto-compactions'\r\n";
            return;
        }
        g_pika_conf->SetDisableAutoCompactions(disable_auto_compactions);
        std::string key = "disable_auto_compactions";
        std::string new_value = disable_auto_compactions ? "true" : "false";
        rocksdb::Status s = g_pika_server->db()->ResetOption(key, new_value);
        if (!s.ok()) {
            ret = "-ERR Reset rocksdb 'disable-auto-compactions' failed\r\n";
            return;
        }
        ret = "+OK\r\n";
    } else if (set_item == "level0-file-num-compaction-trigger") {
        if (!slash::string2l(value.data(), value.size(), &ival)) {
            ret = "-ERR Invalid argument " + value + " for CONFIG SET 'level0-file-num-compaction-trigger'\r\n";
            return;
        }
        g_pika_conf->SetLevel0FileNumCompactionTrigger(ival);
        rocksdb::Status s = g_pika_server->db()->ResetOption("level0_file_num_compaction_trigger", slash::StringToLower(value));
        if (!s.ok()) {
            ret = "-ERR Reset rocksdb 'level0-file-num-compaction-trigger' failed\r\n";
            return;
        }
        ret = "+OK\r\n";
    } else if (set_item == "level0-slowdown-writes-trigger") {
        if (!slash::string2l(value.data(), value.size(), &ival)) {
            ret = "-ERR Invalid argument " + value + " for CONFIG SET 'level0-slowdown-writes-trigger'\r\n";
            return;
        }
        g_pika_conf->SetLevel0SlowdownWritesTrigger(ival);
        rocksdb::Status s = g_pika_server->db()->ResetOption("level0_slowdown_writes_trigger", slash::StringToLower(value));
        if (!s.ok()) {
            ret = "-ERR Reset rocksdb 'level0-slowdown-writes-trigger' failed\r\n";
            return;
        }
        ret = "+OK\r\n";
    } else if (set_item == "level0-stop-writes-trigger") {
        if (!slash::string2l(value.data(), value.size(), &ival) ||  ival <= 0) {
            ret = "-ERR Invalid argument " + value + " for CONFIG SET 'level0-stop-writes-trigger'\r\n";
            return;
        }
        g_pika_conf->SetLevel0StopWritesTrigger(ival);
        rocksdb::Status s = g_pika_server->db()->ResetOption("level0_stop_writes_trigger", slash::StringToLower(value));
        if (!s.ok()) {
            ret = "-ERR Reset rocksdb 'level0-stop-writes-trigger' failed\r\n";
            return;
        }
        ret = "+OK\r\n";
    } else if (set_item == "slave-priority") {
        if (!slash::string2l(value.data(), value.size(), &ival) ||  ival <= 0) {
            ret = "-ERR Invalid argument " + value + " for CONFIG SET 'slave-priority'\r\n";
            return;
        }
        g_pika_conf->SetSlavePriority(ival);
        ret = "+OK\r\n";
    } else if (set_item == "cache-num") {
        if (!slash::string2l(value.data(), value.size(), &ival) || ival < 0) {
            ret = "-ERR Invalid argument " + value + " for CONFIG SET 'cache-num'\r\n";
            return;
        }

        int cache_num = (0 >= ival || 48 < ival) ? 16 : ival;
        if (cache_num != g_pika_conf->cache_num()) {
            g_pika_conf->SetCacheNum(cache_num);
            g_pika_server->ResetCacheAsync(cache_num);
        }
        ret = "+OK\r\n";
    } else if (set_item == "cache-model") {
        if (!slash::string2l(value.data(), value.size(), &ival) || ival < 0) {
            ret = "-ERR Invalid argument " + value + " for CONFIG SET 'cache-model'\r\n";
            return;
        }
        if (PIKA_CACHE_NONE > ival || PIKA_CACHE_READ < ival) {
            ret = "-ERR Invalid cache model\r\n";
        } else {
            g_pika_conf->SetCacheModel(ival);
            if (PIKA_CACHE_NONE == ival) {
                g_pika_server->ClearCacheDbAsync();
            }
            ret = "+OK\r\n";
        }
    } else if (set_item == "cache-maxmemory") {
        if (!slash::string2l(value.data(), value.size(), &ival) || ival < 0) {
            ret = "-ERR Invalid argument " + value + " for CONFIG SET 'cache-maxmemory'\r\n";
            return;
        }
        int64_t cache_maxmemory = (PIKA_CACHE_SIZE_MIN > ival) ? PIKA_CACHE_SIZE_DEFAULT : ival;
        g_pika_conf->SetCacheMaxmemory(cache_maxmemory);
        g_pika_server->ResetCacheConfig();
        ret = "+OK\r\n";
    } else if (set_item == "cache-maxmemory-policy") {
        if (!slash::string2l(value.data(), value.size(), &ival) || ival < 0) {
            ret = "-ERR Invalid argument " + value + " for CONFIG SET 'cache-maxmemory-policy'\r\n";
            return;
        }
        int cache_maxmemory_policy_ = (0 > ival || 5 < ival) ? 3 : ival; // default allkeys-lru
        g_pika_conf->SetCacheMaxmemoryPolicy(cache_maxmemory_policy_);
        g_pika_server->ResetCacheConfig();
        ret = "+OK\r\n";
    } else if (set_item == "cache-maxmemory-samples") {
        if (!slash::string2l(value.data(), value.size(), &ival) || ival < 0) {
            ret = "-ERR Invalid argument " + value + " for CONFIG SET 'cache-maxmemory-samples'\r\n";
            return;
        }
        int cache_maxmemory_samples = (1 > ival) ? 5 : ival;
        g_pika_conf->SetCacheMaxmemorySamples(cache_maxmemory_samples);
        g_pika_server->ResetCacheConfig();
        ret = "+OK\r\n";
    } else if (set_item == "cache-lfu-decay-time") {
        if (!slash::string2l(value.data(), value.size(), &ival) || ival < 0) {
            ret = "-ERR Invalid argument " + value + " for CONFIG SET 'cache-lfu-decay-time'\r\n";
            return;
        }
        int cache_lfu_decay_time = (0 > ival) ? 1 : ival;
        g_pika_conf->SetCacheLFUDecayTime(cache_lfu_decay_time);
        g_pika_server->ResetCacheConfig();
        ret = "+OK\r\n";
    } else {
        ret = "-ERR No such configure item\r\n";
    }
}

void ConfigCmd::ConfigRewrite(std::string &ret) {
    g_pika_conf->ConfigRewrite();
    ret = "+OK\r\n";
}

void ConfigCmd::ConfigResetstat(std::string &ret) {
    g_pika_server->ResetStat();
    ret = "+OK\r\n";
}

void MonitorCmd::DoInitial(PikaCmdArgsType &argv, const CmdInfo* const ptr_info) {
    (void)ptr_info;
    if (argv.size() != 1) {
        res_.SetRes(CmdRes::kWrongNum, kCmdNameMonitor);
        return;
    }
}

void MonitorCmd::Do() {
}

void DbsizeCmd::DoInitial(PikaCmdArgsType &argv, const CmdInfo* const ptr_info) {
    (void)ptr_info;
    if (argv.size() != 1) {
        res_.SetRes(CmdRes::kWrongNum, kCmdNameDbsize);
        return;
    }
}

void DbsizeCmd::Do() {
    if (g_pika_conf->slotmigrate()){
        int64_t dbsize = 0;
        for (int i = 0; i < HASH_SLOTS_SIZE; ++i){
            int32_t card = 0;
            rocksdb::Status s = g_pika_server->db()->SCard(SlotKeyPrefix+std::to_string(i), &card);
            //card = g_pika_server->db()->SCard(SlotKeyPrefix+std::to_string(i));
            if (s.ok() && card >= 0) {
                dbsize += card;
            }else {
                res_.SetRes(CmdRes::kErrOther, "Get dbsize error");
                return;
            }
        }
        res_.AppendInteger(dbsize);
        return;
    }

    PikaServer::KeyScanInfo key_scan_info = g_pika_server->key_scan_info();
    std::vector<uint64_t> &key_nums_v = key_scan_info.key_nums_v;
    if (key_scan_info.key_nums_v.size() != 5) {
        res_.SetRes(CmdRes::kErrOther, "keyspace error");
        return;
    }
    int64_t dbsize = key_nums_v[0] + key_nums_v[1] + key_nums_v[2] + key_nums_v[3] + key_nums_v[4];
    res_.AppendInteger(dbsize);
}

void TimeCmd::DoInitial(PikaCmdArgsType &argv, const CmdInfo* const ptr_info) {
    (void)ptr_info;
    if (argv.size() != 1) {
        res_.SetRes(CmdRes::kWrongNum, kCmdNameTime);
        return;
    }
}

void TimeCmd::Do() {
    struct timeval tv;
    if (gettimeofday(&tv, NULL) == 0) {
        res_.AppendArrayLen(2);
        char buf[32];
        int32_t len = slash::ll2string(buf, sizeof(buf), tv.tv_sec);
        res_.AppendStringLen(len);
        res_.AppendContent(buf);

        len = slash::ll2string(buf, sizeof(buf), tv.tv_usec);
        res_.AppendStringLen(len);
        res_.AppendContent(buf);
    } else {
        res_.SetRes(CmdRes::kErrOther, strerror(errno));
    }
}

void DelbackupCmd::DoInitial(PikaCmdArgsType &argv, const CmdInfo* const ptr_info) {
    (void)ptr_info;
    if (argv.size() != 1) {
        res_.SetRes(CmdRes::kWrongNum, kCmdNameDelbackup);
        return;
    }
}

void DelbackupCmd::Do() {
    std::string db_sync_prefix = g_pika_conf->bgsave_prefix();
    std::string db_sync_path = g_pika_conf->bgsave_path();
    std::vector<std::string> dump_dir;

    // Dump file is not exist
    if (!slash::FileExists(db_sync_path)) {
        res_.SetRes(CmdRes::kOk);
        return;
    }
    // Directory traversal
    if (slash::GetChildren(db_sync_path, dump_dir) != 0) {
        res_.SetRes(CmdRes::kOk);
        return;
    }

    int len = dump_dir.size();
    for (size_t i = 0; i < dump_dir.size(); i++) {
        if (dump_dir[i].substr(0, db_sync_prefix.size()) != db_sync_prefix || dump_dir[i].size() != (db_sync_prefix.size() + 8)) {
            continue;
        }

        std::string str_date = dump_dir[i].substr(db_sync_prefix.size(), (dump_dir[i].size() - db_sync_prefix.size()));
        char *end = NULL;
        std::strtol(str_date.c_str(), &end, 10);
        if (*end != 0) {
            continue;
        }

        std::string dump_dir_name = db_sync_path + dump_dir[i];
        if (g_pika_server->CountSyncSlaves() == 0) {
            LOG(INFO) << "Not syncing, delete dump file: " << dump_dir_name;
            slash::DeleteDirIfExist(dump_dir_name);
            len--;
        } else if (g_pika_server->bgsave_info().path != dump_dir_name){
            LOG(INFO) << "Syncing, delete expired dump file: " << dump_dir_name;
            slash::DeleteDirIfExist(dump_dir_name);
            len--;
        } else {
            LOG(INFO) << "Syncing, can not delete " << dump_dir_name << " dump file" << std::endl;
        }
    }
    if (len == 0) {
        g_pika_server->bgsave_info().Clear();
    }

    res_.SetRes(CmdRes::kOk);
    return;
}

#ifdef TCMALLOC_EXTENSION
void TcmallocCmd::DoInitial(PikaCmdArgsType &argv, const CmdInfo* const ptr_info) {
    (void)ptr_info;
    if (argv.size() != 2 && argv.size() != 3) {
        res_.SetRes(CmdRes::kWrongNum, kCmdNameTcmalloc);
        return;
    }
    rate_ = 0;
    std::string type = slash::StringToLower(argv[1]);
    if (type == "stats") {
        type_ = 0;
    } else if (type == "rate") {
        type_ = 1;
        if (argv.size() == 3) {
            if (!slash::string2l(argv[2].data(), argv[2].size(), &rate_)) {
                res_.SetRes(CmdRes::kSyntaxErr, kCmdNameTcmalloc);
            }
        }
    } else if (type == "list") {
        type_ = 2;
    } else if (type == "free") {
        type_ = 3;
    } else {
        res_.SetRes(CmdRes::kInvalidParameter, kCmdNameTcmalloc);
        return;
    }

}

void TcmallocCmd::Do() {
    std::vector<MallocExtension::FreeListInfo> fli;
    std::vector<std::string> elems;
    switch(type_) {
        case 0:
            char stats[1024];
            MallocExtension::instance()->GetStats(stats, 1024);
            slash::StringSplit(stats, '\n', elems);
            res_.AppendArrayLen(elems.size());
            for (auto& i : elems) {
                res_.AppendString(i);
            }
            break;
        case 1:
            if (rate_) {
                MallocExtension::instance()->SetMemoryReleaseRate(rate_);
            }
            res_.AppendInteger(MallocExtension::instance()->GetMemoryReleaseRate());
            break;
        case 2:
            MallocExtension::instance()->GetFreeListSizes(&fli);
            res_.AppendArrayLen(fli.size());
            for (auto& i : fli) {
                res_.AppendString("type: " + std::string(i.type) + ", min: " + std::to_string(i.min_object_size) +
                    ", max: " + std::to_string(i.max_object_size) + ", total: " + std::to_string(i.total_bytes_free));
            }
            break;
        case 3:
            MallocExtension::instance()->ReleaseFreeMemory();
            res_.SetRes(CmdRes::kOk);
    }
}
#endif

void EchoCmd::DoInitial(PikaCmdArgsType &argv, const CmdInfo* const ptr_info) {
  if (!ptr_info->CheckArg(argv.size())) {
    res_.SetRes(CmdRes::kWrongNum, kCmdNameEcho);
    return;
  }

  echomsg_ = argv[1];
}
void EchoCmd::Do() {
  res_.AppendString(echomsg_);
}

void SlowlogCmd::DoInitial(PikaCmdArgsType &argv, const CmdInfo* const ptr_info)
{
    if (!ptr_info->CheckArg(argv.size())) {
        res_.SetRes(CmdRes::kWrongNum, kCmdNameSlowlog);
        return;
    }

    if (argv.size() == 2 && !strcasecmp(argv[1].data(), "reset")) {
        condition_ = SlowlogCmd::kRESET;
    }
    else if (argv.size() == 2 && !strcasecmp(argv[1].data(), "len")) {
        condition_ = SlowlogCmd::kLEN;
    }
    else if ((argv.size() == 2 || argv.size() == 3) && !strcasecmp(argv[1].data(), "get")) {
        condition_ = SlowlogCmd::kGET;
        if (argv.size() == 3 && !slash::string2l(argv[2].data(), argv[2].size(), &number_)) {
            res_.SetRes(CmdRes::kInvalidInt);
            return;
        }
    }
    else {
        res_.SetRes(CmdRes::kErrOther, "Unknown SLOWLOG subcommand or wrong # of args. Try GET, RESET, LEN.");
        return;
    }
}

void SlowlogCmd::Do() {
    if (condition_ == SlowlogCmd::kRESET) {
        g_pika_server->SlowlogReset();
        res_.SetRes(CmdRes::kOk);
    }
    else if (condition_ ==  SlowlogCmd::kLEN) {
        res_.AppendInteger(g_pika_server->SlowlogLen());
    }
    else {
        std::vector<SlowlogEntry> slowlogs;
        g_pika_server->SlowlogObtain(number_, &slowlogs);
        res_.AppendArrayLen(slowlogs.size());
        for (const auto& slowlog : slowlogs) {
            res_.AppendArrayLen(4);
            res_.AppendInteger(slowlog.id);
            res_.AppendInteger(slowlog.start_time);
            res_.AppendInteger(slowlog.duration);
            res_.AppendArrayLen(slowlog.argv.size());
            for (const auto& arg : slowlog.argv) {
                res_.AppendString(arg);
            }
        }
    }
    return;
}

void CacheCmd::DoInitial(PikaCmdArgsType &argv, const CmdInfo* const ptr_info) {
  if (!ptr_info->CheckArg(argv.size())) {
    res_.SetRes(CmdRes::kWrongNum, kCmdNameCache);
    return;
  }

  if (!strcasecmp(argv[1].data(), "clear")) {
    if (!strcasecmp(argv[2].data(), "db")) {
      condition_ = kCLEAR_DB;
    } else if (!strcasecmp(argv[2].data(), "hitratio")) {
      condition_ = kCLEAR_HITRATIO;
    } else {
      res_.SetRes(CmdRes::kErrOther, "Unknown cache subcommand or wrong # of args.");
    }
  } else if (!strcasecmp(argv[1].data(), "del")) {
    condition_ = kDEL_KEYS;
    std::vector<std::string>::iterator iter = argv.begin();
    keys_.assign(iter + 2, argv.end());
  } else if (!strcasecmp(argv[1].data(), "randomkey")) {
    condition_ = kRANDOM_KEY;
  } else {
    res_.SetRes(CmdRes::kErrOther, "Unknown cache subcommand or wrong # of args.");
  }
  return;
}

void CacheCmd::Do() {
  slash::Status s;
  std::string key;
  switch (condition_) {
    case kCLEAR_DB:
      g_pika_server->ClearCacheDbAsync();
      res_.SetRes(CmdRes::kOk);
      break;
    case kCLEAR_HITRATIO:
      g_pika_server->ClearHitRatio();
      res_.SetRes(CmdRes::kOk);
      break;
    case kDEL_KEYS:
      for (auto& key : keys_) {
        g_pika_server->Cache()->Del(key);
      }
      res_.SetRes(CmdRes::kOk);
      break;
    case kRANDOM_KEY:
      s = g_pika_server->Cache()->RandomKey(&key);
      if (!s.ok()) {
        res_.AppendStringLen(-1);
      } else {
        res_.AppendStringLen(key.size());
        res_.AppendContent(key);
      } 
      break;
    default:
      res_.SetRes(CmdRes::kErrOther, "Unknown cmd");
      break;
  }
  return;
}