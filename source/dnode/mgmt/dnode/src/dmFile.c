/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#define _DEFAULT_SOURCE
#include "dmFile.h"

int32_t dmReadFile(SDnode *pDnode) {
  SDnodeMgmt *pMgmt = &pDnode->dmgmt;

  pMgmt->pDnodeEps = taosArrayInit(1, sizeof(SDnodeEp));
  if (pMgmt->pDnodeEps == NULL) {
    dError("failed to calloc dnodeEp array since %s", strerror(errno));
    goto PRASE_DNODE_OVER;
  }

  int32_t code = TSDB_CODE_DND_DNODE_READ_FILE_ERROR;
  int32_t len = 0;
  int32_t maxLen = 256 * 1024;
  char   *content = calloc(1, maxLen + 1);
  cJSON  *root = NULL;

  char file[PATH_MAX];
  snprintf(file, sizeof(file), "%s%sdnode.json", pDnode->path, TD_DIRSEP);

  TdFilePtr pFile = taosOpenFile(file, TD_FILE_READ);
  if (pFile == NULL) {
    dDebug("file %s not exist", file);
    code = 0;
    goto PRASE_DNODE_OVER;
  }

  len = (int32_t)taosReadFile(pFile, content, maxLen);
  if (len <= 0) {
    dError("failed to read %s since content is null", file);
    goto PRASE_DNODE_OVER;
  }

  content[len] = 0;
  root = cJSON_Parse(content);
  if (root == NULL) {
    dError("failed to read %s since invalid json format", file);
    goto PRASE_DNODE_OVER;
  }

  cJSON *dnodeId = cJSON_GetObjectItem(root, "dnodeId");
  if (!dnodeId || dnodeId->type != cJSON_Number) {
    dError("failed to read %s since dnodeId not found", file);
    goto PRASE_DNODE_OVER;
  }
  pMgmt->dnodeId = dnodeId->valueint;

  cJSON *clusterId = cJSON_GetObjectItem(root, "clusterId");
  if (!clusterId || clusterId->type != cJSON_String) {
    dError("failed to read %s since clusterId not found", file);
    goto PRASE_DNODE_OVER;
  }
  pMgmt->clusterId = atoll(clusterId->valuestring);

  cJSON *dropped = cJSON_GetObjectItem(root, "dropped");
  if (!dropped || dropped->type != cJSON_Number) {
    dError("failed to read %s since dropped not found", file);
    goto PRASE_DNODE_OVER;
  }
  pMgmt->dropped = dropped->valueint;

  cJSON *dnodes = cJSON_GetObjectItem(root, "dnodes");
  if (!dnodes || dnodes->type != cJSON_Array) {
    dError("failed to read %s since dnodes not found", file);
    goto PRASE_DNODE_OVER;
  }

  int32_t numOfDnodes = cJSON_GetArraySize(dnodes);
  if (numOfDnodes <= 0) {
    dError("failed to read %s since numOfDnodes:%d invalid", file, numOfDnodes);
    goto PRASE_DNODE_OVER;
  }

  for (int32_t i = 0; i < numOfDnodes; ++i) {
    cJSON *node = cJSON_GetArrayItem(dnodes, i);
    if (node == NULL) break;

    SDnodeEp dnodeEp = {0};

    cJSON *did = cJSON_GetObjectItem(node, "id");
    if (!did || did->type != cJSON_Number) {
      dError("failed to read %s since dnodeId not found", file);
      goto PRASE_DNODE_OVER;
    }

    dnodeEp.id = dnodeId->valueint;

    cJSON *dnodeFqdn = cJSON_GetObjectItem(node, "fqdn");
    if (!dnodeFqdn || dnodeFqdn->type != cJSON_String || dnodeFqdn->valuestring == NULL) {
      dError("failed to read %s since dnodeFqdn not found", file);
      goto PRASE_DNODE_OVER;
    }
    tstrncpy(dnodeEp.ep.fqdn, dnodeFqdn->valuestring, TSDB_FQDN_LEN);

    cJSON *dnodePort = cJSON_GetObjectItem(node, "port");
    if (!dnodePort || dnodePort->type != cJSON_Number) {
      dError("failed to read %s since dnodePort not found", file);
      goto PRASE_DNODE_OVER;
    }

    dnodeEp.ep.port = dnodePort->valueint;

    cJSON *isMnode = cJSON_GetObjectItem(node, "isMnode");
    if (!isMnode || isMnode->type != cJSON_Number) {
      dError("failed to read %s since isMnode not found", file);
      goto PRASE_DNODE_OVER;
    }
    dnodeEp.isMnode = isMnode->valueint;

    taosArrayPush(pMgmt->pDnodeEps, &dnodeEp);
  }

  code = 0;
  dInfo("succcessed to read file %s", file);
  dndPrintDnodes(pDnode);

PRASE_DNODE_OVER:
  if (content != NULL) free(content);
  if (root != NULL) cJSON_Delete(root);
  if (pFile != NULL) taosCloseFile(&pFile);

  if (dndIsEpChanged(pDnode, pMgmt->dnodeId, pDnode->cfg.localEp)) {
    dError("localEp %s different with %s and need reconfigured", pDnode->cfg.localEp, file);
    return -1;
  }

  if (taosArrayGetSize(pMgmt->pDnodeEps) == 0) {
    SDnodeEp dnodeEp = {0};
    dnodeEp.isMnode = 1;
    taosGetFqdnPortFromEp(pDnode->cfg.firstEp, &dnodeEp.ep);
    taosArrayPush(pMgmt->pDnodeEps, &dnodeEp);
  }

  dndResetDnodes(pDnode, pMgmt->pDnodeEps);

  terrno = 0;
  return 0;
}

int32_t dmWriteFile(SDnode *pDnode) {
  SDnodeMgmt *pMgmt = &pDnode->dmgmt;

  char file[PATH_MAX];
  snprintf(file, sizeof(file), "%s%sdnode.json.bak", pDnode->path, TD_DIRSEP);

  TdFilePtr pFile = taosOpenFile(file, TD_FILE_CTEATE | TD_FILE_WRITE | TD_FILE_TRUNC);
  if (pFile == NULL) {
    dError("failed to write %s since %s", file, strerror(errno));
    terrno = TAOS_SYSTEM_ERROR(errno);
    return -1;
  }

  int32_t len = 0;
  int32_t maxLen = 256 * 1024;
  char   *content = calloc(1, maxLen + 1);

  len += snprintf(content + len, maxLen - len, "{\n");
  len += snprintf(content + len, maxLen - len, "  \"dnodeId\": %d,\n", pMgmt->dnodeId);
  len += snprintf(content + len, maxLen - len, "  \"clusterId\": \"%" PRId64 "\",\n", pMgmt->clusterId);
  len += snprintf(content + len, maxLen - len, "  \"dropped\": %d,\n", pMgmt->dropped);
  len += snprintf(content + len, maxLen - len, "  \"dnodes\": [{\n");

  int32_t numOfEps = (int32_t)taosArrayGetSize(pMgmt->pDnodeEps);
  for (int32_t i = 0; i < numOfEps; ++i) {
    SDnodeEp *pDnodeEp = taosArrayGet(pMgmt->pDnodeEps, i);
    len += snprintf(content + len, maxLen - len, "    \"id\": %d,\n", pDnodeEp->id);
    len += snprintf(content + len, maxLen - len, "    \"fqdn\": \"%s\",\n", pDnodeEp->ep.fqdn);
    len += snprintf(content + len, maxLen - len, "    \"port\": %u,\n", pDnodeEp->ep.port);
    len += snprintf(content + len, maxLen - len, "    \"isMnode\": %d\n", pDnodeEp->isMnode);
    if (i < numOfEps - 1) {
      len += snprintf(content + len, maxLen - len, "  },{\n");
    } else {
      len += snprintf(content + len, maxLen - len, "  }]\n");
    }
  }
  len += snprintf(content + len, maxLen - len, "}\n");

  taosWriteFile(pFile, content, len);
  taosFsyncFile(pFile);
  taosCloseFile(&pFile);
  free(content);

  char realfile[PATH_MAX];
  snprintf(realfile, sizeof(realfile), "%s%smnode.json", pDnode->path, TD_DIRSEP);

  if (taosRenameFile(file, realfile) != 0) {
    terrno = TSDB_CODE_DND_MNODE_WRITE_FILE_ERROR;
    dError("failed to rename %s since %s", file, terrstr());
    return -1;
  }

  pMgmt->updateTime = taosGetTimestampMs();
  dDebug("successed to write %s", file);
  return 0;
}

void dndUpdateDnodeEps(SDnode *pDnode, SArray *pDnodeEps) {
  int32_t numOfEps = taosArrayGetSize(pDnodeEps);
  if (numOfEps <= 0) return;

  SDnodeMgmt *pMgmt = &pDnode->dmgmt;
  taosWLockLatch(&pMgmt->latch);

  int32_t numOfEpsOld = (int32_t)taosArrayGetSize(pMgmt->pDnodeEps);
  if (numOfEps != numOfEpsOld) {
    dndResetDnodes(pDnode, pDnodeEps);
    dmWriteFile(pDnode);
  } else {
    int32_t size = numOfEps * sizeof(SDnodeEp);
    if (memcmp(pMgmt->pDnodeEps->pData, pDnodeEps->pData, size) != 0) {
      dndResetDnodes(pDnode, pDnodeEps);
      dmWriteFile(pDnode);
    }
  }

  taosWUnLockLatch(&pMgmt->latch);
}

void dndResetDnodes(SDnode *pDnode, SArray *pDnodeEps) {
  SDnodeMgmt *pMgmt = &pDnode->dmgmt;

  if (pMgmt->pDnodeEps != pDnodeEps) {
    SArray *tmp = pMgmt->pDnodeEps;
    pMgmt->pDnodeEps = taosArrayDup(pDnodeEps);
    taosArrayDestroy(tmp);
  }

  pMgmt->mnodeEpSet.inUse = 0;
  pMgmt->mnodeEpSet.numOfEps = 0;

  int32_t mIndex = 0;
  int32_t numOfEps = (int32_t)taosArrayGetSize(pDnodeEps);

  for (int32_t i = 0; i < numOfEps; i++) {
    SDnodeEp *pDnodeEp = taosArrayGet(pDnodeEps, i);
    if (!pDnodeEp->isMnode) continue;
    if (mIndex >= TSDB_MAX_REPLICA) continue;
    pMgmt->mnodeEpSet.numOfEps++;

    pMgmt->mnodeEpSet.eps[mIndex] = pDnodeEp->ep;
    mIndex++;
  }

  for (int32_t i = 0; i < numOfEps; i++) {
    SDnodeEp *pDnodeEp = taosArrayGet(pDnodeEps, i);
    taosHashPut(pMgmt->dnodeHash, &pDnodeEp->id, sizeof(int32_t), pDnodeEp, sizeof(SDnodeEp));
  }

  dndPrintDnodes(pDnode);
}

void dndPrintDnodes(SDnode *pDnode) {
  SDnodeMgmt *pMgmt = &pDnode->dmgmt;

  int32_t numOfEps = (int32_t)taosArrayGetSize(pMgmt->pDnodeEps);
  dDebug("print dnode ep list, num:%d", numOfEps);
  for (int32_t i = 0; i < numOfEps; i++) {
    SDnodeEp *pEp = taosArrayGet(pMgmt->pDnodeEps, i);
    dDebug("dnode:%d, fqdn:%s port:%u isMnode:%d", pEp->id, pEp->ep.fqdn, pEp->ep.port, pEp->isMnode);
  }
}

bool dndIsEpChanged(SDnode *pDnode, int32_t dnodeId, char *pEp) {
  bool changed = false;

  SDnodeMgmt *pMgmt = &pDnode->dmgmt;
  taosRLockLatch(&pMgmt->latch);

  SDnodeEp *pDnodeEp = taosHashGet(pMgmt->dnodeHash, &dnodeId, sizeof(int32_t));
  if (pDnodeEp != NULL) {
    char epstr[TSDB_EP_LEN + 1];
    snprintf(epstr, TSDB_EP_LEN, "%s:%u", pDnodeEp->ep.fqdn, pDnodeEp->ep.port);
    changed = strcmp(pEp, epstr) != 0;
  }

  taosRUnLockLatch(&pMgmt->latch);
  return changed;
}