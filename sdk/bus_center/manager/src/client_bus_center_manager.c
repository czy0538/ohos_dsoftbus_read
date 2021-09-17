/*
 * Copyright (c) 2021 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "client_bus_center_manager.h"

#include <pthread.h>
#include <securec.h>

#include "bus_center_server_proxy.h"
#include "common_list.h"
#include "softbus_adapter_mem.h"
#include "softbus_errcode.h"
#include "softbus_feature_config.h"
#include "softbus_log.h"

#define DEFAULT_NODE_STATE_CB_CNT 10

static int32_t g_maxNodeStateCbCount;

typedef struct {
    ListNode node;
    ConnectionAddr addr;
    OnJoinLNNResult cb;
} JoinLNNCbListItem;

typedef struct {
    ListNode node;
    char networkId[NETWORK_ID_BUF_LEN];
    OnLeaveLNNResult cb;
} LeaveLNNCbListItem;

typedef struct {
    ListNode node;
    char networkId[NETWORK_ID_BUF_LEN];
    ITimeSyncCb cb;
} TimeSyncCallbackItem;

typedef struct {
    ListNode node;
    INodeStateCb cb;
} NodeStateCallbackItem;

typedef struct {
    ListNode joinLNNCbList;
    ListNode leaveLNNCbList;
    ListNode nodeStateCbList;
    ListNode timeSyncCbList;
    int32_t nodeStateCbListCnt;
    bool isInit;
    pthread_mutex_t lock;
} BusCenterClient;

static BusCenterClient g_busCenterClient = {
    .nodeStateCbListCnt = 0,
    .isInit = false,
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

static bool IsSameConnectionAddr(const ConnectionAddr *addr1, const ConnectionAddr *addr2)
{
    if (addr1->type != addr2->type) {
        return false;
    }
    if (addr1->type == CONNECTION_ADDR_BR) {
        return strncmp(addr1->info.br.brMac, addr2->info.br.brMac, BT_MAC_LEN) == 0;
    }
    if (addr1->type == CONNECTION_ADDR_BLE) {
        return strncmp(addr1->info.ble.bleMac, addr2->info.ble.bleMac, BT_MAC_LEN) == 0;
    }
    if (addr1->type == CONNECTION_ADDR_WLAN || addr1->type == CONNECTION_ADDR_ETH) {
        return (strncmp(addr1->info.ip.ip, addr2->info.ip.ip, strlen(addr1->info.ip.ip)) == 0)
            && (addr1->info.ip.port == addr2->info.ip.port);
    }
    return false;
}

static JoinLNNCbListItem *FindJoinLNNCbItem(ConnectionAddr *addr, OnJoinLNNResult cb)
{
    JoinLNNCbListItem *item = NULL;

    LIST_FOR_EACH_ENTRY(item, &g_busCenterClient.joinLNNCbList, JoinLNNCbListItem, node) {
        if (IsSameConnectionAddr(&item->addr, addr) &&
            (cb == NULL || cb == item->cb)) {
            return item;
        }
    }
    return NULL;
}

static int32_t AddJoinLNNCbItem(ConnectionAddr *target, OnJoinLNNResult cb)
{
    JoinLNNCbListItem *item = NULL;

    item = (JoinLNNCbListItem *)SoftBusMalloc(sizeof(*item));
    if (item == NULL) {
        SoftBusLog(SOFTBUS_LOG_LNN, SOFTBUS_LOG_ERROR, "fail: malloc join LNN cb list item");
        return SOFTBUS_MALLOC_ERR;
    }
    ListInit(&item->node);
    item->addr = *target;
    item->cb = cb;
    ListAdd(&g_busCenterClient.joinLNNCbList, &item->node);
    return SOFTBUS_OK;
}

static LeaveLNNCbListItem *FindLeaveLNNCbItem(const char *networkId, OnLeaveLNNResult cb)
{
    LeaveLNNCbListItem *item = NULL;

    LIST_FOR_EACH_ENTRY(item, &g_busCenterClient.leaveLNNCbList, LeaveLNNCbListItem, node) {
        if (strcmp(item->networkId, networkId) == 0 &&
            (cb == NULL || cb == item->cb)) {
            return item;
        }
    }
    return NULL;
}

static int32_t AddLeaveLNNCbItem(const char *networkId, OnLeaveLNNResult cb)
{
    LeaveLNNCbListItem *item = NULL;

    item = (LeaveLNNCbListItem *)SoftBusMalloc(sizeof(*item));
    if (item == NULL) {
        SoftBusLog(SOFTBUS_LOG_LNN, SOFTBUS_LOG_ERROR, "fail: malloc join LNN cb list item");
        return SOFTBUS_MALLOC_ERR;
    }
    ListInit(&item->node);
    if (strncpy_s(item->networkId, NETWORK_ID_BUF_LEN, networkId, strlen(networkId)) != EOK) {
        SoftBusLog(SOFTBUS_LOG_LNN, SOFTBUS_LOG_ERROR, "strcpy network id fail");
        SoftBusFree(item);
        return SOFTBUS_ERR;
    }
    item->cb = cb;
    ListAdd(&g_busCenterClient.leaveLNNCbList, &item->node);
    return SOFTBUS_OK;
}

static TimeSyncCallbackItem *FindTimeSyncCbItem(const char *networkId, ITimeSyncCb *cb)
{
    TimeSyncCallbackItem *item = NULL;

    LIST_FOR_EACH_ENTRY(item, &g_busCenterClient.timeSyncCbList, TimeSyncCallbackItem, node) {
        if (strcmp(item->networkId, networkId) == 0 &&
            (cb == NULL || cb->onTimeSyncResult == item->cb.onTimeSyncResult)) {
            return item;
        }
    }
    return NULL;
}

static int32_t AddTimeSyncCbItem(const char *networkId, ITimeSyncCb *cb)
{
    TimeSyncCallbackItem *item = NULL;

    item = (TimeSyncCallbackItem *)SoftBusMalloc(sizeof(*item));
    if (item == NULL) {
        SoftBusLog(SOFTBUS_LOG_LNN, SOFTBUS_LOG_ERROR, "fail: malloc time sync cb list item");
        return SOFTBUS_MALLOC_ERR;
    }
    ListInit(&item->node);
    if (strncpy_s(item->networkId, NETWORK_ID_BUF_LEN, networkId, strlen(networkId)) != EOK) {
        SoftBusLog(SOFTBUS_LOG_LNN, SOFTBUS_LOG_ERROR, "strcpy network id fail");
        SoftBusFree(item);
        return SOFTBUS_ERR;
    }
    item->cb = *cb;
    ListAdd(&g_busCenterClient.timeSyncCbList, &item->node);
    return SOFTBUS_OK;
}

static void ClearJoinLNNList(void)
{
    JoinLNNCbListItem *item = NULL;
    JoinLNNCbListItem *next = NULL;

    LIST_FOR_EACH_ENTRY_SAFE(item, next, &g_busCenterClient.joinLNNCbList, JoinLNNCbListItem, node) {
        ListDelete(&item->node);
        SoftBusFree(item);
    }
}

static void ClearLeaveLNNList(void)
{
    LeaveLNNCbListItem *item = NULL;
    LeaveLNNCbListItem *next = NULL;

    LIST_FOR_EACH_ENTRY_SAFE(item, next, &g_busCenterClient.leaveLNNCbList, LeaveLNNCbListItem, node) {
        ListDelete(&item->node);
        SoftBusFree(item);
    }
}

static void ClearNodeStateCbList(ListNode *list)
{
    NodeStateCallbackItem *item = NULL;
    NodeStateCallbackItem *next = NULL;

    LIST_FOR_EACH_ENTRY_SAFE(item, next, list, NodeStateCallbackItem, node) {
        ListDelete(&item->node);
        SoftBusFree(item);
    }
}

static void DuplicateNodeStateCbList(ListNode *list)
{
    NodeStateCallbackItem *item = NULL;
    NodeStateCallbackItem *copyItem = NULL;

    LIST_FOR_EACH_ENTRY(item, &g_busCenterClient.nodeStateCbList, NodeStateCallbackItem, node) {
        copyItem = SoftBusMalloc(sizeof(NodeStateCallbackItem));
        if (copyItem == NULL) {
            SoftBusLog(SOFTBUS_LOG_LNN, SOFTBUS_LOG_ERROR, "malloc node state callback item fail");
            continue;
        }
        ListInit(&copyItem->node);
        copyItem->cb = item->cb;
        ListAdd(list, &copyItem->node);
    }
}

static void DuplicateTimeSyncResultCbList(ListNode *list, const char *networkId)
{
    TimeSyncCallbackItem *item = NULL;
    TimeSyncCallbackItem *copyItem = NULL;

    LIST_FOR_EACH_ENTRY(item, &g_busCenterClient.timeSyncCbList, TimeSyncCallbackItem, node) {
        if (strcmp(item->networkId, networkId) != 0) {
            continue;
        }
        copyItem = SoftBusMalloc(sizeof(TimeSyncCallbackItem));
        if (copyItem == NULL) {
            SoftBusLog(SOFTBUS_LOG_LNN, SOFTBUS_LOG_ERROR, "malloc time sync callback item fail");
            continue;
        }
        copyItem->cb = item->cb;
        ListInit(&copyItem->node);
        if (strncpy_s(copyItem->networkId, NETWORK_ID_BUF_LEN, item->networkId, strlen(item->networkId)) != EOK) {
            SoftBusLog(SOFTBUS_LOG_LNN, SOFTBUS_LOG_ERROR, "copy networkId fail");
            SoftBusFree(copyItem);
            continue;
        }
        ListAdd(list, &copyItem->node);
    }
}

void BusCenterClientDeinit(void)
{
    if (pthread_mutex_lock(&g_busCenterClient.lock) != 0) {
        SoftBusLog(SOFTBUS_LOG_LNN, SOFTBUS_LOG_ERROR, "fail: lock in deinit");
    }
    ClearJoinLNNList();
    ClearLeaveLNNList();
    ClearNodeStateCbList(&g_busCenterClient.nodeStateCbList);
    g_busCenterClient.nodeStateCbListCnt = 0;
    if (pthread_mutex_unlock(&g_busCenterClient.lock) != 0) {
        SoftBusLog(SOFTBUS_LOG_LNN, SOFTBUS_LOG_ERROR, "fail: unlock in deinit");
    }
}

int BusCenterClientInit(void)
{
    if (SoftbusGetConfig(SOFTBUS_INT_MAX_NODE_STATE_CB_CNT,
        (unsigned char*)&g_maxNodeStateCbCount, sizeof(g_maxNodeStateCbCount)) != SOFTBUS_OK) {
        SoftBusLog(SOFTBUS_LOG_LNN, SOFTBUS_LOG_ERROR, "Cannot get NodeStateCbCount from config file");
        g_maxNodeStateCbCount = DEFAULT_NODE_STATE_CB_CNT;
    }
    SoftBusLog(SOFTBUS_LOG_LNN, SOFTBUS_LOG_INFO, "NodeStateCbCount is %u", g_maxNodeStateCbCount);

    ListInit(&g_busCenterClient.joinLNNCbList);
    ListInit(&g_busCenterClient.leaveLNNCbList);
    ListInit(&g_busCenterClient.nodeStateCbList);
    ListInit(&g_busCenterClient.timeSyncCbList);
    g_busCenterClient.isInit = true;
    if (BusCenterServerProxyInit() != SOFTBUS_OK) {
        SoftBusLog(SOFTBUS_LOG_LNN, SOFTBUS_LOG_ERROR, "bus center server proxy init failed.");
        BusCenterClientDeinit();
        return SOFTBUS_ERR;
    }
    SoftBusLog(SOFTBUS_LOG_LNN, SOFTBUS_LOG_INFO, "BusCenterClientInit init OK!");
    return SOFTBUS_OK;
}

int32_t GetAllNodeDeviceInfoInner(const char *pkgName, NodeBasicInfo **info, int32_t *infoNum)
{
    int ret = ServerIpcGetAllOnlineNodeInfo(pkgName, (void **)info, sizeof(NodeBasicInfo), infoNum);
    if (ret != SOFTBUS_OK) {
        SoftBusLog(SOFTBUS_LOG_LNN, SOFTBUS_LOG_ERROR, "Server GetAllOnlineNodeInfo failed, ret = %d", ret);
    }
    return ret;
}

int32_t GetLocalNodeDeviceInfoInner(const char *pkgName, NodeBasicInfo *info)
{
    int ret = ServerIpcGetLocalDeviceInfo(pkgName, info, sizeof(*info));
    if (ret != SOFTBUS_OK) {
        SoftBusLog(SOFTBUS_LOG_LNN, SOFTBUS_LOG_ERROR, "Server GetLocalNodeDeviceInfo failed, ret = %d", ret);
    }
    return ret;
}

int32_t GetNodeKeyInfoInner(const char *pkgName, const char *networkId, NodeDeivceInfoKey key,
    uint8_t *info, int32_t infoLen)
{
    int ret = ServerIpcGetNodeKeyInfo(pkgName, networkId, key, info, infoLen);
    if (ret != SOFTBUS_OK) {
        SoftBusLog(SOFTBUS_LOG_LNN, SOFTBUS_LOG_ERROR, "Server GetNodeKeyInfo failed, ret = %d", ret);
    }
    return ret;
}

int32_t JoinLNNInner(const char *pkgName, ConnectionAddr *target, OnJoinLNNResult cb)
{
    int32_t rc;

    if (!g_busCenterClient.isInit) {
        SoftBusLog(SOFTBUS_LOG_LNN, SOFTBUS_LOG_ERROR, "fail : join lnn not init");
        return SOFTBUS_ERR;
    }
    if (pthread_mutex_lock(&g_busCenterClient.lock) != 0) {
        SoftBusLog(SOFTBUS_LOG_LNN, SOFTBUS_LOG_ERROR, "fail: lock join lnn cb list in join");
    }
    rc = SOFTBUS_ERR;
    do {
        if (FindJoinLNNCbItem(target, cb) != NULL) {
            SoftBusLog(SOFTBUS_LOG_LNN, SOFTBUS_LOG_ERROR, "fail : join request already exist");
            break;
        }
        rc = ServerIpcJoinLNN(pkgName, target, sizeof(*target));
        if (rc != SOFTBUS_OK) {
            SoftBusLog(SOFTBUS_LOG_LNN, SOFTBUS_LOG_ERROR, "fail : request join lnn");
        } else {
            rc = AddJoinLNNCbItem(target, cb);
        }
    } while (false);
    if (pthread_mutex_unlock(&g_busCenterClient.lock) != 0) {
        SoftBusLog(SOFTBUS_LOG_LNN, SOFTBUS_LOG_ERROR, "fail: unlock join lnn cb list in join");
    }
    return rc;
}

int32_t LeaveLNNInner(const char *pkgName, const char *networkId, OnLeaveLNNResult cb)
{
    int32_t rc;

    if (!g_busCenterClient.isInit) {
        SoftBusLog(SOFTBUS_LOG_LNN, SOFTBUS_LOG_ERROR, "fail : leave lnn not init");
        return SOFTBUS_ERR;
    }
    if (pthread_mutex_lock(&g_busCenterClient.lock) != 0) {
        SoftBusLog(SOFTBUS_LOG_LNN, SOFTBUS_LOG_ERROR, "fail: lock leave lnn cb list in leave");
    }
    rc = SOFTBUS_ERR;
    do {
        if (FindLeaveLNNCbItem(networkId, cb) != NULL) {
            SoftBusLog(SOFTBUS_LOG_LNN, SOFTBUS_LOG_ERROR, "fail : leave request already exist");
            break;
        }
        rc = ServerIpcLeaveLNN(pkgName, networkId);
        if (rc != SOFTBUS_OK) {
            SoftBusLog(SOFTBUS_LOG_LNN, SOFTBUS_LOG_ERROR, "fail : request leave lnn");
        } else {
            rc = AddLeaveLNNCbItem(networkId, cb);
        }
    } while (false);
    if (pthread_mutex_unlock(&g_busCenterClient.lock) != 0) {
        SoftBusLog(SOFTBUS_LOG_LNN, SOFTBUS_LOG_ERROR, "fail: unlock leave lnn cb list in leave");
    }
    return rc;
}

int32_t RegNodeDeviceStateCbInner(const char *pkgName, INodeStateCb *callback)
{
    NodeStateCallbackItem *item = NULL;
    int32_t rc = SOFTBUS_ERR;

    if (!g_busCenterClient.isInit) {
        SoftBusLog(SOFTBUS_LOG_LNN, SOFTBUS_LOG_ERROR, "fail: reg node state cb not init");
        return SOFTBUS_ERR;
    }
    if (pthread_mutex_lock(&g_busCenterClient.lock) != 0) {
        SoftBusLog(SOFTBUS_LOG_LNN, SOFTBUS_LOG_ERROR, "fail: lock node state cb list in reg");
    }
    do {
        if (g_busCenterClient.nodeStateCbListCnt >= g_maxNodeStateCbCount) {
            break;
        }
        item = (NodeStateCallbackItem *)SoftBusMalloc(sizeof(*item));
        if (item == NULL) {
            rc = SOFTBUS_MALLOC_ERR;
            break;
        }
        ListInit(&item->node);
        item->cb = *callback;
        ListAdd(&g_busCenterClient.nodeStateCbList, &item->node);
        g_busCenterClient.nodeStateCbListCnt++;
        rc = SOFTBUS_OK;
        item = NULL;
    } while (false);
    if (pthread_mutex_unlock(&g_busCenterClient.lock) != 0) {
        SoftBusLog(SOFTBUS_LOG_LNN, SOFTBUS_LOG_ERROR, "fail: unlock node state cb list");
    }
    if (item != NULL) {
        SoftBusFree(item);
    }
    return rc;
}

int32_t UnregNodeDeviceStateCbInner(INodeStateCb *callback)
{
    NodeStateCallbackItem *item = NULL;
    NodeStateCallbackItem *next = NULL;

    if (!g_busCenterClient.isInit) {
        SoftBusLog(SOFTBUS_LOG_LNN, SOFTBUS_LOG_ERROR, "fail: unreg node state cb not init");
        return SOFTBUS_ERR;
    }
    if (pthread_mutex_lock(&g_busCenterClient.lock) != 0) {
        SoftBusLog(SOFTBUS_LOG_LNN, SOFTBUS_LOG_ERROR, "fail: lock node state cb list in unreg");
    }
    LIST_FOR_EACH_ENTRY_SAFE(item, next, &g_busCenterClient.nodeStateCbList, NodeStateCallbackItem, node) {
        if (memcmp(&item->cb, callback, sizeof(*callback)) == 0) {
            ListDelete(&item->node);
            SoftBusFree(item);
            g_busCenterClient.nodeStateCbListCnt--;
            break;
        }
    }
    if (pthread_mutex_unlock(&g_busCenterClient.lock) != 0) {
        SoftBusLog(SOFTBUS_LOG_LNN, SOFTBUS_LOG_ERROR, "fail: unlock node state cb list in unreg");
    }
    return SOFTBUS_OK;
}

int32_t StartTimeSyncInner(const char *pkgName, const char *targetNetworkId, TimeSyncAccuracy accuracy,
    TimeSyncPeriod period, ITimeSyncCb *cb)
{
    int32_t rc;

    if (!g_busCenterClient.isInit) {
        SoftBusLog(SOFTBUS_LOG_LNN, SOFTBUS_LOG_ERROR, "fail : start time sync not init");
        return SOFTBUS_ERR;
    }
    if (pthread_mutex_lock(&g_busCenterClient.lock) != 0) {
        SoftBusLog(SOFTBUS_LOG_LNN, SOFTBUS_LOG_ERROR, "fail: lock time sync cb list");
    }
    rc = SOFTBUS_ERR;
    do {
        if (FindTimeSyncCbItem(targetNetworkId, cb) != NULL) {
            SoftBusLog(SOFTBUS_LOG_LNN, SOFTBUS_LOG_ERROR, "repeat request from %s, StopTimeSync first!", pkgName);
            break;
        }
        rc = ServerIpcStartTimeSync(pkgName, targetNetworkId, accuracy, period);
        if (rc != SOFTBUS_OK) {
            SoftBusLog(SOFTBUS_LOG_LNN, SOFTBUS_LOG_ERROR, "fail : start time sync");
        } else {
            rc = AddTimeSyncCbItem(targetNetworkId, cb);
        }
    } while (false);
    if (pthread_mutex_unlock(&g_busCenterClient.lock) != 0) {
        SoftBusLog(SOFTBUS_LOG_LNN, SOFTBUS_LOG_ERROR, "fail: unlock time sync cb list");
    }
    return rc;
}

int32_t StopTimeSyncInner(const char *pkgName, const char *targetNetworkId)
{
    int32_t rc;
    TimeSyncCallbackItem *item = NULL;

    if (!g_busCenterClient.isInit) {
        SoftBusLog(SOFTBUS_LOG_LNN, SOFTBUS_LOG_ERROR, "fail : stop time sync cb list not init");
        return SOFTBUS_ERR;
    }
    if (pthread_mutex_lock(&g_busCenterClient.lock) != 0) {
        SoftBusLog(SOFTBUS_LOG_LNN, SOFTBUS_LOG_ERROR, "fail: lock time sync cb list");
    }
    rc = SOFTBUS_ERR;
    while ((item = FindTimeSyncCbItem(targetNetworkId, NULL)) != NULL) {
        rc = ServerIpcStopTimeSync(pkgName, targetNetworkId);
        if (rc != SOFTBUS_OK) {
            SoftBusLog(SOFTBUS_LOG_LNN, SOFTBUS_LOG_ERROR, "fail : stop time sync");
        } else {
            ListDelete(&item->node);
            SoftBusFree(item);
        }
    }

    if (pthread_mutex_unlock(&g_busCenterClient.lock) != 0) {
        SoftBusLog(SOFTBUS_LOG_LNN, SOFTBUS_LOG_ERROR, "fail: unlock time sync cb list");
    }
    return rc;
}

int32_t LnnOnJoinResult(void *addr, const char *networkId, int32_t retCode)
{
    JoinLNNCbListItem *item = NULL;
    ConnectionAddr *connAddr = (ConnectionAddr *)addr;

    if (connAddr == NULL) {
        return SOFTBUS_INVALID_PARAM;
    }
    if (!g_busCenterClient.isInit) {
        return SOFTBUS_ERR;
    }

    if (pthread_mutex_lock(&g_busCenterClient.lock) != 0) {
        SoftBusLog(SOFTBUS_LOG_LNN, SOFTBUS_LOG_ERROR, "fail: lock join lnn cb list in join result");
    }
    while ((item = FindJoinLNNCbItem(addr, NULL)) != NULL) {
        ListDelete(&item->node);
        if (pthread_mutex_unlock(&g_busCenterClient.lock) != 0) {
            SoftBusLog(SOFTBUS_LOG_LNN, SOFTBUS_LOG_ERROR, "fail: unlock join lnn cb list in join result");
        }
        if (item->cb != NULL) {
            item->cb(connAddr, networkId, retCode);
        }
        SoftBusFree(item);
        if (pthread_mutex_lock(&g_busCenterClient.lock) != 0) {
            SoftBusLog(SOFTBUS_LOG_LNN, SOFTBUS_LOG_ERROR, "fail: lock join lnn cb list in join result");
        }
    }
    if (pthread_mutex_unlock(&g_busCenterClient.lock) != 0) {
        SoftBusLog(SOFTBUS_LOG_LNN, SOFTBUS_LOG_ERROR, "fail: unlock join lnn cb list in join result");
    }
    return SOFTBUS_OK;
}

int32_t LnnOnLeaveResult(const char *networkId, int32_t retCode)
{
    LeaveLNNCbListItem *item = NULL;

    if (networkId == NULL) {
        SoftBusLog(SOFTBUS_LOG_LNN, SOFTBUS_LOG_ERROR, "fail: networkId is null");
        return SOFTBUS_INVALID_PARAM;
    }
    if (!g_busCenterClient.isInit) {
        SoftBusLog(SOFTBUS_LOG_LNN, SOFTBUS_LOG_ERROR, "fail: leave cb not init");
        return SOFTBUS_ERR;
    }

    if (pthread_mutex_lock(&g_busCenterClient.lock) != 0) {
        SoftBusLog(SOFTBUS_LOG_LNN, SOFTBUS_LOG_ERROR, "fail: lock leave lnn cb list in leave result");
    }
    while ((item = FindLeaveLNNCbItem(networkId, NULL)) != NULL) {
        ListDelete(&item->node);
        if (pthread_mutex_unlock(&g_busCenterClient.lock) != 0) {
            SoftBusLog(SOFTBUS_LOG_LNN, SOFTBUS_LOG_ERROR, "fail: unlock leave lnn cb list in leave result");
        }
        if (item->cb != NULL) {
            item->cb(networkId, retCode);
        }
        SoftBusFree(item);
        if (pthread_mutex_lock(&g_busCenterClient.lock) != 0) {
            SoftBusLog(SOFTBUS_LOG_LNN, SOFTBUS_LOG_ERROR, "fail: lock leave lnn cb list in leave result");
        }
    }
    if (pthread_mutex_unlock(&g_busCenterClient.lock) != 0) {
        SoftBusLog(SOFTBUS_LOG_LNN, SOFTBUS_LOG_ERROR, "fail: unlock leave lnn cb list in leave result");
    }
    return SOFTBUS_OK;
}

int32_t LnnOnNodeOnlineStateChanged(bool isOnline, void *info)
{
    NodeStateCallbackItem *item = NULL;
    NodeBasicInfo *basicInfo = (NodeBasicInfo *)info;
    ListNode dupList;

    if (basicInfo == NULL) {
        return SOFTBUS_INVALID_PARAM;
    }
    if (!g_busCenterClient.isInit) {
        return SOFTBUS_ERR;
    }

    if (pthread_mutex_lock(&g_busCenterClient.lock) != 0) {
        SoftBusLog(SOFTBUS_LOG_LNN, SOFTBUS_LOG_ERROR, "fail: lock node state cb list in notify");
    }
    ListInit(&dupList);
    DuplicateNodeStateCbList(&dupList);
    if (pthread_mutex_unlock(&g_busCenterClient.lock) != 0) {
        SoftBusLog(SOFTBUS_LOG_LNN, SOFTBUS_LOG_ERROR, "fail: unlock node state cb list in notify");
    }
    LIST_FOR_EACH_ENTRY(item, &dupList, NodeStateCallbackItem, node) {
        if (isOnline == true) {
            if ((item->cb.events & EVENT_NODE_STATE_ONLINE) != 0) {
                item->cb.onNodeOnline(basicInfo);
            }
        } else {
            if ((item->cb.events & EVENT_NODE_STATE_OFFLINE) != 0) {
                item->cb.onNodeOffline(basicInfo);
            }
        }
    }
    ClearNodeStateCbList(&dupList);
    return SOFTBUS_OK;
}

int32_t LnnOnNodeBasicInfoChanged(void *info, int32_t type)
{
    NodeStateCallbackItem *item = NULL;
    NodeBasicInfo *basicInfo = (NodeBasicInfo *)info;
    ListNode dupList;

    if (basicInfo == NULL) {
        SoftBusLog(SOFTBUS_LOG_LNN, SOFTBUS_LOG_ERROR, "info or list is null");
        return SOFTBUS_INVALID_PARAM;
    }
    if (!g_busCenterClient.isInit) {
        return SOFTBUS_ERR;
    }

    if ((type < 0) || (type > TYPE_DEVICE_NAME)) {
        SoftBusLog(SOFTBUS_LOG_LNN, SOFTBUS_LOG_ERROR, "OnNodeBasicInfoChanged invalid type: %d", type);
        return SOFTBUS_INVALID_PARAM;
    }

    if (pthread_mutex_lock(&g_busCenterClient.lock) != 0) {
        SoftBusLog(SOFTBUS_LOG_LNN, SOFTBUS_LOG_ERROR, "fail: lock node basic info cb list in notify");
    }
    ListInit(&dupList);
    DuplicateNodeStateCbList(&dupList);
    if (pthread_mutex_unlock(&g_busCenterClient.lock) != 0) {
        SoftBusLog(SOFTBUS_LOG_LNN, SOFTBUS_LOG_ERROR, "fail: unlock node basic info cb list in notify");
    }
    LIST_FOR_EACH_ENTRY(item, &dupList, NodeStateCallbackItem, node) {
        if ((item->cb.events & EVENT_NODE_STATE_INFO_CHANGED) != 0) {
            item->cb.onNodeBasicInfoChanged(type, basicInfo);
        }
    }
    ClearNodeStateCbList(&dupList);
    return SOFTBUS_OK;
}

int32_t LnnOnTimeSyncResult(const void *info, int retCode)
{
    TimeSyncCallbackItem *item = NULL;
    TimeSyncResultInfo *basicInfo = (TimeSyncResultInfo *)info;
    ListNode dupList;

    if (info == NULL) {
        SoftBusLog(SOFTBUS_LOG_LNN, SOFTBUS_LOG_ERROR, "info or list is null");
        return SOFTBUS_INVALID_PARAM;
    }
    if (!g_busCenterClient.isInit) {
        SoftBusLog(SOFTBUS_LOG_LNN, SOFTBUS_LOG_ERROR, "fail: time sync cb not init");
        return SOFTBUS_ERR;
    }

    if (pthread_mutex_lock(&g_busCenterClient.lock) != 0) {
        SoftBusLog(SOFTBUS_LOG_LNN, SOFTBUS_LOG_ERROR, "fail: lock time sync cb list in time sync result");
    }
    ListInit(&dupList);
    DuplicateTimeSyncResultCbList(&dupList, basicInfo->target.targetNetworkId);
    if (pthread_mutex_unlock(&g_busCenterClient.lock) != 0) {
        SoftBusLog(SOFTBUS_LOG_LNN, SOFTBUS_LOG_ERROR, "fail: unlock time sync cb list in time sync result");
    }
    LIST_FOR_EACH_ENTRY(item, &dupList, TimeSyncCallbackItem, node) {
        if (item->cb.onTimeSyncResult != NULL) {
            item->cb.onTimeSyncResult(info, retCode);
        }
    }
    return SOFTBUS_OK;
}