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
#include "dndExec.h"

void dndGenerateGrant() {
#if 0
  grantParseParameter();
#endif
}

void dndPrintVersion() {
#ifdef TD_ENTERPRISE
  char *releaseName = "enterprise";
#else
  char *releaseName = "community";
#endif
  printf("%s version: %s compatible_version: %s\n", releaseName, version, compatible_version);
  printf("gitinfo: %s\n", gitinfo);
  printf("builuInfo: %s\n", buildinfo);
}

void dndDumpCfg() {
  SConfig *pCfg = taosGetCfg();
  cfgDumpCfg(pCfg, 0, 1);
}

SDndCfg dndGetCfg() {
  SConfig *pCfg = taosGetCfg();
  SDndCfg  objCfg = {0};

  objCfg.numOfSupportVnodes = cfgGetItem(pCfg, "supportVnodes")->i32;
  tstrncpy(objCfg.dataDir, tsDataDir, sizeof(objCfg.dataDir));
  tstrncpy(objCfg.firstEp, tsFirst, sizeof(objCfg.firstEp));
  tstrncpy(objCfg.secondEp, tsSecond, sizeof(objCfg.firstEp));
  objCfg.serverPort = tsServerPort;
  tstrncpy(objCfg.localFqdn, tsLocalFqdn, sizeof(objCfg.localFqdn));
  snprintf(objCfg.localEp, sizeof(objCfg.localEp), "%s:%u", objCfg.localFqdn, objCfg.serverPort);
  objCfg.pDisks = tsDiskCfg;
  objCfg.numOfDisks = tsDiskCfgNum;
  return objCfg;
}