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

#include "bus_center_server_proxy.h"

#include "securec.h"

#include "lnn_bus_center_ipc.h"
#include "softbus_def.h"
#include "softbus_errcode.h"
#include "softbus_ipc_def.h"
#include "softbus_log.h"

int32_t BusCenterServerProxyInit(void)
{
    LOG_INFO("bus center get server proxy ok");
    return SOFTBUS_OK;
}

int ServerIpcGetAllOnlineNodeInfo(const char *pkgName, void **info, uint32_t infoTypeLen, int32_t *infoNum)
{
    return LnnIpcGetAllOnlineNodeInfo(pkgName, info, infoTypeLen, infoNum);
}

int32_t ServerIpcGetLocalDeviceInfo(const char *pkgName, void *info, uint32_t infoTypeLen)
{
    return LnnIpcGetLocalDeviceInfo(pkgName, info, infoTypeLen);
}

int32_t ServerIpcGetNodeKeyInfo(const char *pkgName, const char *networkId, int key, unsigned char *buf, uint32_t len)
{
    return LnnIpcGetNodeKeyInfo(pkgName, networkId, key, buf, len);
}

int ServerIpcJoinLNN(const char *pkgName, void *addr, unsigned int addrTypeLen)
{
    return LnnIpcServerJoin(pkgName, addr, addrTypeLen);
}

int ServerIpcLeaveLNN(const char *pkgName, const char *networkId)
{
    return LnnIpcServerLeave(pkgName, networkId);
}
