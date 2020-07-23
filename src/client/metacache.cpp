/*
 *  Copyright (c) 2020 NetEase Inc.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

/*
 * Project: curve
 * File Created: Tuesday, 25th September 2018 2:06:35 pm
 * Author: tongguangxun
 */
#include <gflags/gflags.h>
#include <glog/logging.h>

#include <bthread/bthread.h>

#include <utility>
#include <algorithm>

#include "proto/cli.pb.h"

#include "src/client/metacache.h"
#include "src/client/mds_client.h"
#include "src/client/client_common.h"
#include "src/common/concurrent/concurrent.h"

using curve::common::WriteLockGuard;
using curve::common::ReadLockGuard;
using curve::client::ClientConfig;

namespace curve {
namespace client {

void MetaCache::Init(MetaCacheOption_t metaCacheOpt, MDSClient* mdsclient) {
    mdsclient_ = mdsclient;
    metacacheopt_ = metaCacheOpt;
    LOG(INFO) << "metacache init success!"
              << ", get leader retry times = "
              << metacacheopt_.metacacheGetLeaderRetry
              << ", get leader retry interval us = "
              << metacacheopt_.metacacheRPCRetryIntervalUS
              << ", get leader rpc time out ms = "
              << metacacheopt_.metacacheGetLeaderRPCTimeOutMS;
}

MetaCacheErrorType MetaCache::GetChunkInfoByIndex(ChunkIndex chunkidx, ChunkIDInfo_t* chunxinfo ) {  // NOLINT
    ReadLockGuard rdlk(rwlock4ChunkInfo_);
    auto iter = chunkindex2idMap_.find(chunkidx);
    if (iter != chunkindex2idMap_.end()) {
        *chunxinfo = iter->second;
        return MetaCacheErrorType::OK;
    }
    return MetaCacheErrorType::CHUNKINFO_NOT_FOUND;
}

bool MetaCache::IsLeaderMayChange(LogicPoolID logicPoolId,
                                  CopysetID copysetId) {
    rwlock4ChunkInfo_.RDLock();
    auto iter = lpcsid2CopsetInfoMap_.find(
        LogicPoolCopysetID2Str(logicPoolId, copysetId));
    if (iter == lpcsid2CopsetInfoMap_.end()) {
        rwlock4ChunkInfo_.Unlock();
        return false;
    }

    bool flag = iter->second.LeaderMayChange();
    rwlock4ChunkInfo_.Unlock();
    return flag;
}

int MetaCache::GetLeader(LogicPoolID logicPoolId,
                        CopysetID copysetId,
                        ChunkServerID* serverId,
                        EndPoint* serverAddr,
                        bool refresh,
                        FileMetric* fm) {
    std::string mapkey = LogicPoolCopysetID2Str(logicPoolId, copysetId);

    CopysetInfo_t targetInfo;
    rwlock4CopysetInfo_.RDLock();
    auto iter = lpcsid2CopsetInfoMap_.find(mapkey);
    if (iter == lpcsid2CopsetInfoMap_.end()) {
        rwlock4CopysetInfo_.Unlock();
        LOG(ERROR) << "server list not exist, LogicPoolID = " << logicPoolId
                   << ", CopysetID = " << copysetId;
        return -1;
    }
    targetInfo = iter->second;
    rwlock4CopysetInfo_.Unlock();

    int ret = 0;
    if (refresh || targetInfo.LeaderMayChange()) {
        uint32_t retry = 0;
        while (retry++ < metacacheopt_.metacacheGetLeaderRetry) {
            ret = UpdateLeaderInternal(logicPoolId, copysetId, &targetInfo, fm);
            if (ret != -1) {
                targetInfo.ResetSetLeaderUnstableFlag();
                UpdateCopysetInfo(logicPoolId, copysetId, targetInfo);
                break;
            }

            LOG(INFO) << "refresh leader from chunkserver failed, "
                      << "get copyset chunkserver list from mds, "
                      << "logicpool id = " << logicPoolId
                      << ", copyset id = " << copysetId;

            // 重试失败，这时候需要向mds重新拉取最新的copyset信息了
            ret = UpdateCopysetInfoFromMDS(logicPoolId, copysetId);
            if (ret == 0) {
                continue;
            }

            bthread_usleep(metacacheopt_.metacacheRPCRetryIntervalUS);
        }
    }

    if (ret == -1) {
        LOG_EVERY_N(ERROR, 10) << "get leader failed after retry!"
            << ", copyset id = " << copysetId
            << ", logicpool id = " << logicPoolId;
        return -1;
    }

    return targetInfo.GetLeaderInfo(serverId, serverAddr);
}

int MetaCache::UpdateLeaderInternal(LogicPoolID logicPoolId,
                                    CopysetID copysetId,
                                    CopysetInfo* toupdateCopyset,
                                    FileMetric* fm) {
    ChunkServerID csid = 0;
    ChunkServerAddr  leaderaddr;
    GetLeaderRpcOption rpcOption(metacacheopt_.metacacheGetLeaderRPCTimeOutMS);
    GetLeaderInfo getLeaderInfo(logicPoolId,
                        copysetId, toupdateCopyset->csinfos_,
                        toupdateCopyset->GetCurrentLeaderIndex(),
                        rpcOption);
    int ret = ServiceHelper::GetLeader(
        getLeaderInfo, &leaderaddr, &csid, fm);

    if (ret == -1) {
        LOG_EVERY_N(ERROR, 10) << "get leader failed!"
            << ", copyset id = " << copysetId
            << ", logicpool id = " << logicPoolId;
        return -1;
    }

    ret = toupdateCopyset->UpdateLeaderInfo(csid, leaderaddr);

    // 如果更新失败，说明chunksderverid不合法，就从mds拉取当前chunkserver的id信息
    if (ret == -1 && !leaderaddr.IsEmpty()) {
        ret = mdsclient_->GetChunkServerID(leaderaddr, &csid);

        if (ret != LIBCURVE_ERROR::OK) {
            LOG(ERROR) << "get chunkserver id from mds failed, addr = "
                       << leaderaddr.ToString();
            return -1;
        }

        UpdateCopysetInfoIfMatchCurrentLeader(
            logicPoolId, copysetId, leaderaddr);
        *toupdateCopyset = GetCopysetinfo(logicPoolId, copysetId);
        ret = toupdateCopyset->UpdateLeaderInfo(csid, leaderaddr);
    }

    return ret;
}

int MetaCache::UpdateCopysetInfoFromMDS(LogicPoolID logicPoolId,
                                         CopysetID copysetId) {
    std::vector<CopysetInfo> copysetInfos;

    int ret =
        mdsclient_->GetServerList(logicPoolId, {copysetId}, &copysetInfos);

    if (copysetInfos.empty()) {
        LOG(WARNING) << "Get copyset server list from mds return empty server "
                        "list, ret = "
                     << ret << ", logicpool id = " << logicPoolId
                     << ", copyset id = " << copysetId;
        return -1;
    }

    // 更新chunkserverid到copyset映射关系
    UpdateChunkserverCopysetInfo(logicPoolId, copysetInfos[0]);
    // 更新logicpool和copysetid到copysetinfo的映射
    UpdateCopysetInfo(logicPoolId, copysetId, copysetInfos[0]);

    return 0;
}

void MetaCache::UpdateCopysetInfoIfMatchCurrentLeader(
    LogicPoolID logicPoolId, CopysetID copysetId,
    const ChunkServerAddr& leaderAddr) {
    std::vector<CopysetInfo> copysetInfos;
    int ret =
        mdsclient_->GetServerList(logicPoolId, {copysetId}, &copysetInfos);

    bool needUpdate = (!copysetInfos.empty()) &&
                      (copysetInfos[0].HasChunkServerInCopyset(leaderAddr));
    if (needUpdate) {
        LOG(INFO) << "Update copyset info"
                  << ", logicpool id = " << logicPoolId
                  << ", copyset id = " << copysetId
                  << ", current leader = " << leaderAddr.ToString();

        // 更新chunkserverid到copyset的映射关系
        UpdateChunkserverCopysetInfo(logicPoolId, copysetInfos[0]);
        // 更新logicpool和copysetid到copysetinfo的映射
        UpdateCopysetInfo(logicPoolId, copysetId, copysetInfos[0]);
    }
}

CopysetInfo_t MetaCache::GetServerList(LogicPoolID logicPoolId,
                                       CopysetID copysetId) {
    std::string mapkey = LogicPoolCopysetID2Str(logicPoolId, copysetId);
    CopysetInfo_t ret;

    ReadLockGuard rdlk(rwlock4CopysetInfo_);
    auto iter = lpcsid2CopsetInfoMap_.find(mapkey);
    if (iter == lpcsid2CopsetInfoMap_.end()) {
        // it's impossible to get here
        return ret;
    }
    return iter->second;
}

/**
 * when copyset client find that the leader is redirect,
 * the copyset client will call UpdateLeader.
 * return the ChunkServerID to invoker
 */
int MetaCache::UpdateLeader(LogicPoolID logicPoolId,
    CopysetID copysetId, ChunkServerID* leaderId, const EndPoint &leaderAddr) {
    std::string mapkey = LogicPoolCopysetID2Str(logicPoolId, copysetId);

    ReadLockGuard rdlk(rwlock4CopysetInfo_);
    auto iter = lpcsid2CopsetInfoMap_.find(mapkey);
    if (iter == lpcsid2CopsetInfoMap_.end()) {
        // it's impossible to get here
        return -1;
    }

    ChunkServerAddr csAddr(leaderAddr);
    return iter->second.UpdateLeaderInfo(*leaderId, csAddr);
}

void MetaCache::UpdateChunkInfoByIndex(ChunkIndex cindex, ChunkIDInfo_t cinfo) {
    WriteLockGuard wrlk(rwlock4ChunkInfo_);
    chunkindex2idMap_[cindex] = cinfo;
}

void MetaCache::UpdateCopysetInfo(LogicPoolID logicPoolid, CopysetID copysetid,
                                  const CopysetInfo& csinfo) {
    auto key = LogicPoolCopysetID2Str(logicPoolid, copysetid);
    WriteLockGuard wrlk(rwlock4CopysetInfo_);
    lpcsid2CopsetInfoMap_[key] = csinfo;
}

void MetaCache::UpdateAppliedIndex(LogicPoolID logicPoolId,
    CopysetID copysetId, uint64_t appliedindex) {
    std::string mapkey = LogicPoolCopysetID2Str(logicPoolId, copysetId);

    WriteLockGuard wrlk(rwlock4CopysetInfo_);
    auto iter = lpcsid2CopsetInfoMap_.find(mapkey);
    if (iter == lpcsid2CopsetInfoMap_.end()) {
        return;
    }
    iter->second.UpdateAppliedIndex(appliedindex);
}

uint64_t MetaCache::GetAppliedIndex(LogicPoolID logicPoolId,
                                    CopysetID copysetId) {
    std::string mapkey = LogicPoolCopysetID2Str(logicPoolId, copysetId);

    ReadLockGuard rdlk(rwlock4CopysetInfo_);
    auto iter = lpcsid2CopsetInfoMap_.find(mapkey);
    if (iter == lpcsid2CopsetInfoMap_.end()) {
        return 0;
    }

    return iter->second.GetAppliedIndex();
}

void MetaCache::UpdateChunkInfoByID(ChunkID cid, ChunkIDInfo cidinfo) {
    WriteLockGuard wrlk(rwlock4chunkInfoMap_);
    chunkid2chunkInfoMap_[cid] = cidinfo;
}

int MetaCache::SetServerUnstable(const std::string& serverIp) {
    LOG(WARNING) << "Server unstable, ip = " << serverIp << "";

    std::vector<ChunkServerID> csIds;
    int ret = mdsclient_->ListChunkServerInServer(serverIp, &csIds);
    if (ret != LIBCURVE_ERROR::OK) {
        LOG(WARNING) << "ListChunkServer failed";
        return -1;
    }

    for (auto id : csIds) {
        SetChunkserverUnstable(id);
    }

    return 0;
}

void MetaCache::SetChunkserverUnstable(ChunkServerID csid) {
    LOG(WARNING) << "chunkserver " << csid << " unstable!";
    std::set<CopysetIDInfo> copysetIDSet;

    {
        ReadLockGuard rdlk(rwlock4CSCopysetIDMap_);
        auto iter = chunkserverCopysetIDMap_.find(csid);
        if (iter != chunkserverCopysetIDMap_.end()) {
            copysetIDSet = iter->second;
        }
    }

    ReadLockGuard rdlk(rwlock4CopysetInfo_);
    for (auto it : copysetIDSet) {
        std::string mapkey = LogicPoolCopysetID2Str(it.lpid, it.cpid);
        auto cpinfo = lpcsid2CopsetInfoMap_.find(mapkey);
        if (cpinfo != lpcsid2CopsetInfoMap_.end()) {
            ChunkServerID leaderid;
            if (cpinfo->second.GetCurrentLeaderServerID(&leaderid)) {
                if (leaderid == csid) {
                    // 只设置leaderid为当前serverid的Lcopyset
                    cpinfo->second.SetLeaderUnstableFlag();
                }
            } else {
                // 当前copyset集群信息未知，直接设置LeaderUnStable
                cpinfo->second.SetLeaderUnstableFlag();
            }
        }
    }
}

void MetaCache::AddCopysetIDInfo(ChunkServerID csid,
    const CopysetIDInfo& cpidinfo) {
    WriteLockGuard wrlk(rwlock4CSCopysetIDMap_);
    chunkserverCopysetIDMap_[csid].emplace(cpidinfo);
}

void MetaCache::UpdateChunkserverCopysetInfo(LogicPoolID lpid,
    const CopysetInfo_t& cpinfo) {
    ReadLockGuard rdlk(rwlock4CopysetInfo_);
    std::string mapkey = LogicPoolCopysetID2Str(lpid, cpinfo.cpid_);
    // 先获取原来的chunkserver到copyset映射
    auto previouscpinfo = lpcsid2CopsetInfoMap_.find(mapkey);
    if (previouscpinfo != lpcsid2CopsetInfoMap_.end()) {
        std::vector<ChunkServerID> newID;
        std::vector<ChunkServerID> changedID;

        // 先判断当前copyset有没有变更chunkserverid
        for (auto iter : previouscpinfo->second.csinfos_) {
            changedID.push_back(iter.chunkserverid_);
        }

        for (auto iter : cpinfo.csinfos_) {
            auto it = std::find(changedID.begin(), changedID.end(),
                                iter.chunkserverid_);
            if (it != changedID.end()) {
                changedID.erase(it);
            } else {
                newID.push_back(iter.chunkserverid_);
            }
        }

        // 删除变更的copyset信息
        for (auto chunkserverid : changedID) {
            {
                WriteLockGuard wrlk(rwlock4CSCopysetIDMap_);
                auto iter = chunkserverCopysetIDMap_.find(chunkserverid);
                if (iter != chunkserverCopysetIDMap_.end()) {
                    iter->second.erase(CopysetIDInfo(lpid, cpinfo.cpid_));
                }
            }
        }

        // 更新新的copyset信息到chunkserver
        for (auto chunkserverid : newID) {
            WriteLockGuard wrlk(rwlock4CSCopysetIDMap_);
            chunkserverCopysetIDMap_[chunkserverid].emplace(lpid, cpinfo.cpid_);  // NOLINT
        }
    }
}

CopysetInfo_t MetaCache::GetCopysetinfo(LogicPoolID lpid, CopysetID csid) {
    ReadLockGuard rdlk(rwlock4CopysetInfo_);
    std::string mapkey = LogicPoolCopysetID2Str(lpid, csid);
    auto cpinfo = lpcsid2CopsetInfoMap_.find(mapkey);
    if (cpinfo != lpcsid2CopsetInfoMap_.end()) {
        return cpinfo->second;
    }
    return CopysetInfo();
}

std::string MetaCache::LogicPoolCopysetChunkID2Str(LogicPoolID lpid,
    CopysetID csid, ChunkID chunkid) {
    return std::to_string(lpid).append("_")
                                .append(std::to_string(csid))
                                .append("_")
                                .append(std::to_string(chunkid));
}
std::string MetaCache::LogicPoolCopysetID2Str(LogicPoolID lpid,
    CopysetID csid) {
    return std::to_string(lpid).append("_").append(std::to_string(csid));
}
}   // namespace client
}   // namespace curve