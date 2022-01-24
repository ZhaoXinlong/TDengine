
#include "../../libs/scheduler/inc/schedulerInt.h"
#include "clientInt.h"
#include "clientLog.h"
#include "parser.h"
#include "planner.h"
#include "scheduler.h"
#include "tdef.h"
#include "tep.h"
#include "tglobal.h"
#include "tmsgtype.h"
#include "tnote.h"
#include "tpagedfile.h"
#include "tref.h"

#define CHECK_CODE_GOTO(expr, label) \
  do {                               \
    int32_t code = expr;             \
    if (TSDB_CODE_SUCCESS != code) { \
      terrno = code;                 \
      goto label;                    \
    }                                \
  } while (0)

static int32_t initEpSetFromCfg(const char *firstEp, const char *secondEp, SCorEpSet *pEpSet);
static SMsgSendInfo* buildConnectMsg(SRequestObj *pRequest);
static void destroySendMsgInfo(SMsgSendInfo* pMsgBody);
static void setQueryResultByRsp(SReqResultInfo* pResultInfo, const SRetrieveTableRsp* pRsp);

  static bool stringLengthCheck(const char* str, size_t maxsize) {
  if (str == NULL) {
    return false;
  }

  size_t len = strlen(str);
  if (len <= 0 || len > maxsize) {
    return false;
  }

  return true;
}

static bool validateUserName(const char* user) {
  return stringLengthCheck(user, TSDB_USER_LEN - 1);
}

static bool validatePassword(const char* passwd) {
  return stringLengthCheck(passwd, TSDB_PASSWORD_LEN - 1);
}

static bool validateDbName(const char* db) {
  return stringLengthCheck(db, TSDB_DB_NAME_LEN - 1);
}

static char* getClusterKey(const char* user, const char* auth, const char* ip, int32_t port) {
  char key[512] = {0};
  snprintf(key, sizeof(key), "%s:%s:%s:%d", user, auth, ip, port);
  return strdup(key);
}

static STscObj* taosConnectImpl(const char *user, const char *auth, const char *db, uint16_t port, __taos_async_fn_t fp, void *param, SAppInstInfo* pAppInfo);
static void  setResSchemaInfo(SReqResultInfo* pResInfo, const SDataBlockSchema* pDataBlockSchema);

TAOS *taos_connect_internal(const char *ip, const char *user, const char *pass, const char *auth, const char *db, uint16_t port) {
  if (taos_init() != TSDB_CODE_SUCCESS) {
    return NULL;
  }

  if (!validateUserName(user)) {
    terrno = TSDB_CODE_TSC_INVALID_USER_LENGTH;
    return NULL;
  }

  char localDb[TSDB_DB_NAME_LEN] = {0};
  if (db != NULL) {
    if(!validateDbName(db)) {
      terrno = TSDB_CODE_TSC_INVALID_DB_LENGTH;
      return NULL;
    }

    tstrncpy(localDb, db, sizeof(localDb));
    strdequote(localDb);
  }

  char secretEncrypt[TSDB_PASSWORD_LEN + 1] = {0};
  if (auth == NULL) {
    if (!validatePassword(pass)) {
      terrno = TSDB_CODE_TSC_INVALID_PASS_LENGTH;
      return NULL;
    }

    taosEncryptPass_c((uint8_t *)pass, strlen(pass), secretEncrypt);
  } else {
    tstrncpy(secretEncrypt, auth, tListLen(secretEncrypt));
  }

  SCorEpSet epSet = {0};
  if (ip) {
    if (initEpSetFromCfg(ip, NULL, &epSet) < 0) {
      return NULL;
    }

    if (port) {
      epSet.epSet.port[0] = port;
    }
  } else {
    if (initEpSetFromCfg(tsFirst, tsSecond, &epSet) < 0) {
      return NULL;
    }
  }

  char* key = getClusterKey(user, secretEncrypt, ip, port);

  // TODO: race condition here.
  SAppInstInfo** pInst = taosHashGet(appInfo.pInstMap, key, strlen(key));
  if (pInst == NULL) {
    SAppInstInfo* p = calloc(1, sizeof(struct SAppInstInfo));
    p->mgmtEp       = epSet;
    p->pTransporter = openTransporter(user, secretEncrypt, tsNumOfCores);
    p->pAppHbMgr = appHbMgrInit(p);
    taosHashPut(appInfo.pInstMap, key, strlen(key), &p, POINTER_BYTES);

    pInst = &p;
  }

  tfree(key);
  return taosConnectImpl(user, &secretEncrypt[0], localDb, port, NULL, NULL, *pInst);
}

int32_t buildRequest(STscObj *pTscObj, const char *sql, int sqlLen, SRequestObj** pRequest) {
  *pRequest = createRequest(pTscObj, NULL, NULL, TSDB_SQL_SELECT);
  if (*pRequest == NULL) {
    tscError("failed to malloc sqlObj");
    return TSDB_CODE_TSC_OUT_OF_MEMORY;
  }

  (*pRequest)->sqlstr = malloc(sqlLen + 1);
  if ((*pRequest)->sqlstr == NULL) {
    tscError("0x%"PRIx64" failed to prepare sql string buffer", (*pRequest)->self);
    (*pRequest)->msgBuf = strdup("failed to prepare sql string buffer");
    return TSDB_CODE_TSC_OUT_OF_MEMORY;
  }

  strntolower((*pRequest)->sqlstr, sql, (int32_t)sqlLen);
  (*pRequest)->sqlstr[sqlLen] = 0;
  (*pRequest)->sqlLen = sqlLen;

  tscDebugL("0x%"PRIx64" SQL: %s, reqId:0x%"PRIx64, (*pRequest)->self, (*pRequest)->sqlstr, (*pRequest)->requestId);
  return TSDB_CODE_SUCCESS;
}

int32_t parseSql(SRequestObj* pRequest, SQueryNode** pQuery) {
  STscObj* pTscObj = pRequest->pTscObj;

  SParseContext cxt = {
    .requestId = pRequest->requestId,
    .acctId    = pTscObj->acctId,
    .db        = getConnectionDB(pTscObj),
    .pSql      = pRequest->sqlstr,
    .sqlLen    = pRequest->sqlLen,
    .pMsg      = pRequest->msgBuf,
    .msgLen    = ERROR_MSG_BUF_DEFAULT_SIZE,
    .pTransporter = pTscObj->pAppInfo->pTransporter,
  };

  cxt.mgmtEpSet = getEpSet_s(&pTscObj->pAppInfo->mgmtEp);
  int32_t code = catalogGetHandle(pTscObj->pAppInfo->clusterId, &cxt.pCatalog);
  if (code != TSDB_CODE_SUCCESS) {
    tfree(cxt.db);
    return code;
  }

  code = qParseQuerySql(&cxt, pQuery);

  tfree(cxt.db);
  return code;
}

int32_t execDdlQuery(SRequestObj* pRequest, SQueryNode* pQuery) {
  SDclStmtInfo* pDcl = (SDclStmtInfo*)pQuery;
  pRequest->type = pDcl->msgType;
  pRequest->body.requestMsg = (SDataBuf){.pData = pDcl->pMsg, .len = pDcl->msgLen};

  STscObj* pTscObj = pRequest->pTscObj;
  SMsgSendInfo* pSendMsg = buildMsgInfoImpl(pRequest);

  int64_t transporterId = 0;
  if (pDcl->msgType == TDMT_VND_CREATE_TABLE || pDcl->msgType == TDMT_VND_SHOW_TABLES) {
    if (pDcl->msgType == TDMT_VND_SHOW_TABLES) {
      SShowReqInfo* pShowReqInfo = &pRequest->body.showInfo;
      if (pShowReqInfo->pArray == NULL) {
        pShowReqInfo->currentIndex = 0;  // set the first vnode/ then iterate the next vnode
        pShowReqInfo->pArray = pDcl->pExtension;
      }
    }
    asyncSendMsgToServer(pTscObj->pAppInfo->pTransporter, &pDcl->epSet, &transporterId, pSendMsg);
  } else {
    asyncSendMsgToServer(pTscObj->pAppInfo->pTransporter, &pDcl->epSet, &transporterId, pSendMsg);
  }

  tsem_wait(&pRequest->body.rspSem);
  return TSDB_CODE_SUCCESS;
}

int32_t getPlan(SRequestObj* pRequest, SQueryNode* pQueryNode, SQueryDag** pDag) {
  pRequest->type = pQueryNode->type;

  SReqResultInfo* pResInfo = &pRequest->body.resInfo;
  int32_t code = qCreateQueryDag(pQueryNode, pDag, pRequest->requestId);
  if (code != 0) {
    return code;
  }

  if (pQueryNode->type == TSDB_SQL_SELECT) {
    SArray* pa = taosArrayGetP((*pDag)->pSubplans, 0);

    SSubplan* pPlan = taosArrayGetP(pa, 0);
    SDataBlockSchema* pDataBlockSchema = &(pPlan->pDataSink->schema);
    setResSchemaInfo(pResInfo, pDataBlockSchema);

    pRequest->type = TDMT_VND_QUERY;
  }

  return code;
}

void setResSchemaInfo(SReqResultInfo* pResInfo, const SDataBlockSchema* pDataBlockSchema) {
  assert(pDataBlockSchema != NULL && pDataBlockSchema->numOfCols > 0);

  pResInfo->numOfCols = pDataBlockSchema->numOfCols;
  pResInfo->fields = calloc(pDataBlockSchema->numOfCols, sizeof(pDataBlockSchema->pSchema[0]));

  for (int32_t i = 0; i < pResInfo->numOfCols; ++i) {
    SSchema* pSchema = &pDataBlockSchema->pSchema[i];
    pResInfo->fields[i].bytes = pSchema->bytes;
    pResInfo->fields[i].type  = pSchema->type;
    tstrncpy(pResInfo->fields[i].name, pSchema->name, tListLen(pResInfo->fields[i].name));
  }
}

int32_t scheduleQuery(SRequestObj* pRequest, SQueryDag* pDag) {
  if (TSDB_SQL_INSERT == pRequest->type || TSDB_SQL_CREATE_TABLE == pRequest->type) {
    SQueryResult res = {.code = 0, .numOfRows = 0, .msgSize = ERROR_MSG_BUF_DEFAULT_SIZE, .msg = pRequest->msgBuf};

    int32_t code = scheduleExecJob(pRequest->pTscObj->pAppInfo->pTransporter, NULL, pDag, &pRequest->body.pQueryJob, &res);
    if (code != TSDB_CODE_SUCCESS) {
      // handle error and retry
    } else {
      if (pRequest->body.pQueryJob != NULL) {
        scheduleFreeJob(pRequest->body.pQueryJob);
      }
    }

    pRequest->body.resInfo.numOfRows = res.numOfRows;
    pRequest->code = res.code;
    return pRequest->code;
  }

  SArray *execNode = taosArrayInit(4, sizeof(SQueryNodeAddr));

  SQueryNodeAddr addr = {.numOfEps = 1, .inUse = 0, .nodeId = 2};
  addr.epAddr[0].port = 7100;
  strcpy(addr.epAddr[0].fqdn, "localhost");

  taosArrayPush(execNode, &addr);
  return scheduleAsyncExecJob(pRequest->pTscObj->pAppInfo->pTransporter, execNode, pDag, &pRequest->body.pQueryJob);
}

typedef struct tmq_t tmq_t;

typedef struct SMqClientTopic {
  // subscribe info
  int32_t sqlLen;
  char*   sql;
  char*   topicName;
  int64_t topicId;
  // statistics
  int64_t consumeCnt;
  // offset
  int64_t committedOffset;
  int64_t currentOffset;
  //connection info
  int32_t vgId;
  SEpSet  epSet;
} SMqClientTopic;

typedef struct tmq_resp_err_t {
  int32_t code;
} tmq_resp_err_t;

typedef struct tmq_topic_vgroup_list_t {
  char* topicName;
  int32_t vgId;
  int64_t committedOffset;
} tmq_topic_vgroup_list_t;

typedef void (tmq_commit_cb(tmq_t*, tmq_resp_err_t, tmq_topic_vgroup_list_t*, void* param));

typedef struct tmq_conf_t{
  char*          clientId;
  char*          groupId;
  char*          ip;
  uint16_t       port;
  tmq_commit_cb* commit_cb;
} tmq_conf_t;

struct tmq_t {
  char           groupId[256];
  char           clientId[256];
  STscObj*       pTscObj;
  tmq_commit_cb* commit_cb;
  SArray*        clientTopics;  // SArray<SMqClientTopic>
};

void tmq_conf_set_offset_commit_cb(tmq_conf_t* conf, tmq_commit_cb* cb) {
  conf->commit_cb = cb;
}

SArray* tmqGetConnInfo(SClientHbKey connKey, void* param) {
  tmq_t* pTmq = (void*)param;
  SArray* pArray = taosArrayInit(0, sizeof(SKv));
  if (pArray == NULL) {
    return NULL;
  }
  SKv kv = {0};
  kv.key = malloc(256);
  if (kv.key == NULL) {
    taosArrayDestroy(pArray);
    return NULL;
  }
  strcpy(kv.key, "mq-tmp");
  kv.keyLen = strlen("mq-tmp") + 1;
  SMqHbMsg* pMqHb = malloc(sizeof(SMqHbMsg));
  if (pMqHb == NULL) {
    return pArray;
  }
  pMqHb->consumerId = connKey.connId;
  SArray* clientTopics = pTmq->clientTopics;
  int sz = taosArrayGetSize(clientTopics);
  for (int i = 0; i < sz; i++) {
    SMqClientTopic* pCTopic = taosArrayGet(clientTopics, i);
    if (pCTopic->vgId == -1) {
      pMqHb->status = 1;
      break;
    }
  }
  kv.value = pMqHb;
  kv.valueLen = sizeof(SMqHbMsg);
  taosArrayPush(pArray, &kv);

  return pArray;
}

tmq_t* tmqCreateConsumerImpl(TAOS* conn, tmq_conf_t* conf) {
  tmq_t* pTmq = malloc(sizeof(tmq_t));
  if (pTmq == NULL) {
    return NULL;
  }
  strcpy(pTmq->groupId, conf->groupId);
  strcpy(pTmq->clientId, conf->clientId);
  pTmq->pTscObj = (STscObj*)conn;
  pTmq->pTscObj->connType = HEARTBEAT_TYPE_MQ;

  return pTmq;
}

TAOS_RES *taos_create_topic(TAOS* taos, const char* topicName, const char* sql, int sqlLen) {
  STscObj     *pTscObj = (STscObj*)taos;
  SRequestObj *pRequest = NULL;
  SQueryNode  *pQueryNode = NULL;
  char        *pStr = NULL;

  terrno = TSDB_CODE_SUCCESS;
  if (taos == NULL || topicName == NULL || sql == NULL) {
    tscError("invalid parameters for creating topic, connObj:%p, topic name:%s, sql:%s", taos, topicName, sql);
    terrno = TSDB_CODE_TSC_INVALID_INPUT;
    goto _return;
  }

  if (strlen(topicName) >= TSDB_TOPIC_NAME_LEN) {
    tscError("topic name too long, max length:%d", TSDB_TOPIC_NAME_LEN - 1);
    terrno = TSDB_CODE_TSC_INVALID_INPUT;
    goto _return;
  }

  if (sqlLen > tsMaxSQLStringLen) {
    tscError("sql string exceeds max length:%d", tsMaxSQLStringLen);
    terrno = TSDB_CODE_TSC_EXCEED_SQL_LIMIT;
    goto _return;
  }

  tscDebug("start to create topic, %s", topicName);

  CHECK_CODE_GOTO(buildRequest(pTscObj, sql, sqlLen, &pRequest), _return);
  CHECK_CODE_GOTO(parseSql(pRequest, &pQueryNode), _return);

  // todo check for invalid sql statement and return with error code

  CHECK_CODE_GOTO(qCreateQueryDag(pQueryNode, &pRequest->body.pDag, pRequest->requestId), _return);

  pStr = qDagToString(pRequest->body.pDag);
  if(pStr == NULL) {
    goto _return;
  }

  // The topic should be related to a database that the queried table is belonged to.
  SName name = {0};
  char dbName[TSDB_DB_FNAME_LEN] = {0};
  tNameGetFullDbName(&((SQueryStmtInfo*) pQueryNode)->pTableMetaInfo[0]->name, dbName);

  tNameFromString(&name, dbName, T_NAME_ACCT|T_NAME_DB);
  tNameFromString(&name, topicName, T_NAME_TABLE);

  char topicFname[TSDB_TOPIC_FNAME_LEN] = {0};
  tNameExtractFullName(&name, topicFname);

  SCMCreateTopicReq req = {
    .name         = (char*) topicFname,
    .igExists     = 0,
    .physicalPlan = (char*) pStr,
    .sql          = (char*) sql,
    .logicalPlan  = "no logic plan",
  };

  int tlen = tSerializeSCMCreateTopicReq(NULL, &req);
  void* buf = malloc(tlen);
  if(buf == NULL) {
    goto _return;
  }

  void* abuf = buf;
  tSerializeSCMCreateTopicReq(&abuf, &req);
  /*printf("formatted: %s\n", dagStr);*/

  pRequest->body.requestMsg = (SDataBuf){ .pData = buf, .len = tlen };
  pRequest->type = TDMT_MND_CREATE_TOPIC;

  SMsgSendInfo* body = buildMsgInfoImpl(pRequest);
  SEpSet epSet = getEpSet_s(&pTscObj->pAppInfo->mgmtEp);

  int64_t transporterId = 0;
  asyncSendMsgToServer(pTscObj->pAppInfo->pTransporter, &epSet, &transporterId, body);

  tsem_wait(&pRequest->body.rspSem);

_return:
  qDestroyQuery(pQueryNode);
  if (body != NULL) {
    destroySendMsgInfo(body);
  }

  if (pRequest != NULL && terrno != TSDB_CODE_SUCCESS) {
    pRequest->code = terrno;
  }

  return pRequest;
}

typedef struct tmq_message_t {
  int32_t  numOfRows;
  char*    topicName;
  TAOS_ROW row[];
} tmq_message_t;

tmq_message_t* tmq_consume_poll(tmq_t* mq, int64_t blocking_time) {
  return NULL;
}

tmq_resp_err_t* tmq_commit(tmq_t* mq, void* callback, int32_t async) {
  return NULL;
}

void tmq_message_destroy(tmq_message_t* mq_message) {

}


TAOS_RES *taos_query_l(TAOS *taos, const char *sql, int sqlLen) {
  STscObj *pTscObj = (STscObj *)taos;
  if (sqlLen > (size_t) tsMaxSQLStringLen) {
    tscError("sql string exceeds max length:%d", tsMaxSQLStringLen);
    terrno = TSDB_CODE_TSC_EXCEED_SQL_LIMIT;
    return NULL;
  }

  nPrintTsc("%s", sql)

  SRequestObj *pRequest = NULL;
  SQueryNode  *pQueryNode = NULL;

  terrno = TSDB_CODE_SUCCESS;
  CHECK_CODE_GOTO(buildRequest(pTscObj, sql, sqlLen, &pRequest), _return);
  CHECK_CODE_GOTO(parseSql(pRequest, &pQueryNode), _return);

  if (qIsDdlQuery(pQueryNode)) {
    CHECK_CODE_GOTO(execDdlQuery(pRequest, pQueryNode), _return);
  } else {
    CHECK_CODE_GOTO(getPlan(pRequest, pQueryNode, &pRequest->body.pDag), _return);
    CHECK_CODE_GOTO(scheduleQuery(pRequest, pRequest->body.pDag), _return);
    pRequest->code = terrno;
  }

_return:
  qDestroyQuery(pQueryNode);
  if (NULL != pRequest && TSDB_CODE_SUCCESS != terrno) {
    pRequest->code = terrno;
  }

  return pRequest;
}

int initEpSetFromCfg(const char *firstEp, const char *secondEp, SCorEpSet *pEpSet) {
  pEpSet->version = 0;

  // init mnode ip set
  SEpSet *mgmtEpSet   = &(pEpSet->epSet);
  mgmtEpSet->numOfEps = 0;
  mgmtEpSet->inUse    = 0;

  if (firstEp && firstEp[0] != 0) {
    if (strlen(firstEp) >= TSDB_EP_LEN) {
      terrno = TSDB_CODE_TSC_INVALID_FQDN;
      return -1;
    }

    taosGetFqdnPortFromEp(firstEp, mgmtEpSet->fqdn[0], &(mgmtEpSet->port[0]));
    mgmtEpSet->numOfEps++;
  }

  if (secondEp && secondEp[0] != 0) {
    if (strlen(secondEp) >= TSDB_EP_LEN) {
      terrno = TSDB_CODE_TSC_INVALID_FQDN;
      return -1;
    }

    taosGetFqdnPortFromEp(secondEp, mgmtEpSet->fqdn[mgmtEpSet->numOfEps], &(mgmtEpSet->port[mgmtEpSet->numOfEps]));
    mgmtEpSet->numOfEps++;
  }

  if (mgmtEpSet->numOfEps == 0) {
    terrno = TSDB_CODE_TSC_INVALID_FQDN;
    return -1;
  }

  return 0;
}

STscObj* taosConnectImpl(const char *user, const char *auth, const char *db, uint16_t port, __taos_async_fn_t fp, void *param, SAppInstInfo* pAppInfo) {
  STscObj *pTscObj = createTscObj(user, auth, db, pAppInfo);
  if (NULL == pTscObj) {
    terrno = TSDB_CODE_TSC_OUT_OF_MEMORY;
    return pTscObj;
  }

  SRequestObj *pRequest = createRequest(pTscObj, fp, param, TDMT_MND_CONNECT);
  if (pRequest == NULL) {
    destroyTscObj(pTscObj);
    terrno = TSDB_CODE_TSC_OUT_OF_MEMORY;
    return NULL;
  }

  SMsgSendInfo* body = buildConnectMsg(pRequest);

  int64_t transporterId = 0;
  asyncSendMsgToServer(pTscObj->pAppInfo->pTransporter, &pTscObj->pAppInfo->mgmtEp.epSet, &transporterId, body);

  tsem_wait(&pRequest->body.rspSem);
  if (pRequest->code != TSDB_CODE_SUCCESS) {
    const char *errorMsg = (pRequest->code == TSDB_CODE_RPC_FQDN_ERROR) ? taos_errstr(pRequest) : tstrerror(terrno);
    printf("failed to connect to server, reason: %s\n\n", errorMsg);

    destroyRequest(pRequest);
    taos_close(pTscObj);
    pTscObj = NULL;
  } else {
    tscDebug("0x%"PRIx64" connection is opening, connId:%d, dnodeConn:%p, reqId:0x%"PRIx64, pTscObj->id, pTscObj->connId, pTscObj->pAppInfo->pTransporter, pRequest->requestId);
    destroyRequest(pRequest);
  }

  return pTscObj;
}

static SMsgSendInfo* buildConnectMsg(SRequestObj *pRequest) {
  SMsgSendInfo *pMsgSendInfo = calloc(1, sizeof(SMsgSendInfo));
  if (pMsgSendInfo == NULL) {
    terrno = TSDB_CODE_TSC_OUT_OF_MEMORY;
    return NULL;
  }

  pMsgSendInfo->msgType         = TDMT_MND_CONNECT;
  pMsgSendInfo->msgInfo.len     = sizeof(SConnectReq);
  pMsgSendInfo->requestObjRefId = pRequest->self;
  pMsgSendInfo->requestId       = pRequest->requestId;
  pMsgSendInfo->fp              = handleRequestRspFp[TMSG_INDEX(pMsgSendInfo->msgType)];
  pMsgSendInfo->param           = pRequest;

  SConnectReq *pConnect = calloc(1, sizeof(SConnectReq));
  if (pConnect == NULL) {
    tfree(pMsgSendInfo);
    terrno = TSDB_CODE_TSC_OUT_OF_MEMORY;
    return NULL;
  }

  STscObj *pObj = pRequest->pTscObj;

  char* db = getConnectionDB(pObj);
  if (db != NULL) {
    tstrncpy(pConnect->db, db, sizeof(pConnect->db));
  }
  tfree(db);

  pConnect->pid = htonl(appInfo.pid);
  pConnect->startTime = htobe64(appInfo.startTime);
  tstrncpy(pConnect->app, appInfo.appName, tListLen(pConnect->app));

  pMsgSendInfo->msgInfo.pData = pConnect;
  return pMsgSendInfo;
}

static void destroySendMsgInfo(SMsgSendInfo* pMsgBody) {
  assert(pMsgBody != NULL);
  tfree(pMsgBody->msgInfo.pData);
  tfree(pMsgBody);
}

void processMsgFromServer(void* parent, SRpcMsg* pMsg, SEpSet* pEpSet) {
  SMsgSendInfo *pSendInfo = (SMsgSendInfo *) pMsg->ahandle;
  assert(pMsg->ahandle != NULL);

  if (pSendInfo->requestObjRefId != 0) {
    SRequestObj *pRequest = (SRequestObj *)taosAcquireRef(clientReqRefPool, pSendInfo->requestObjRefId);
    assert(pRequest->self == pSendInfo->requestObjRefId);

    pRequest->metric.rsp = taosGetTimestampMs();
    pRequest->code = pMsg->code;

    STscObj *pTscObj = pRequest->pTscObj;
    if (pEpSet) {
      if (!isEpsetEqual(&pTscObj->pAppInfo->mgmtEp.epSet, pEpSet)) {
        updateEpSet_s(&pTscObj->pAppInfo->mgmtEp, pEpSet);
      }
    }

    /*
   * There is not response callback function for submit response.
   * The actual inserted number of points is the first number.
     */
    int32_t elapsed = pRequest->metric.rsp - pRequest->metric.start;
    if (pMsg->code == TSDB_CODE_SUCCESS) {
      tscDebug("0x%" PRIx64 " message:%s, code:%s rspLen:%d, elapsed:%d ms, reqId:0x%"PRIx64, pRequest->self,
          TMSG_INFO(pMsg->msgType), tstrerror(pMsg->code), pMsg->contLen, elapsed, pRequest->requestId);
    } else {
      tscError("0x%" PRIx64 " SQL cmd:%s, code:%s rspLen:%d, elapsed time:%d ms, reqId:0x%"PRIx64, pRequest->self,
          TMSG_INFO(pMsg->msgType), tstrerror(pMsg->code), pMsg->contLen, elapsed, pRequest->requestId);
    }

    taosReleaseRef(clientReqRefPool, pSendInfo->requestObjRefId);
  }

  SDataBuf buf = {.len = pMsg->contLen, .pData = NULL};

  if (pMsg->contLen > 0) {
    buf.pData = calloc(1, pMsg->contLen);
    if (buf.pData == NULL) {
      terrno = TSDB_CODE_OUT_OF_MEMORY;
      pMsg->code = TSDB_CODE_OUT_OF_MEMORY;
    } else {
      memcpy(buf.pData, pMsg->pCont, pMsg->contLen);
    }
  }

  pSendInfo->fp(pSendInfo->param, &buf, pMsg->code);
  rpcFreeCont(pMsg->pCont);
  destroySendMsgInfo(pSendInfo);
}

TAOS *taos_connect_auth(const char *ip, const char *user, const char *auth, const char *db, uint16_t port) {
  tscDebug("try to connect to %s:%u by auth, user:%s db:%s", ip, port, user, db);
  if (user == NULL) {
    user = TSDB_DEFAULT_USER;
  }

  if (auth == NULL) {
    tscError("No auth info is given, failed to connect to server");
    return NULL;
  }

  return taos_connect_internal(ip, user, NULL, auth, db, port);
}

TAOS *taos_connect_l(const char *ip, int ipLen, const char *user, int userLen, const char *pass, int passLen, const char *db, int dbLen, uint16_t port) {
  char ipStr[TSDB_EP_LEN]      = {0};
  char dbStr[TSDB_DB_NAME_LEN] = {0};
  char userStr[TSDB_USER_LEN]  = {0};
  char passStr[TSDB_PASSWORD_LEN]   = {0};

  strncpy(ipStr,   ip,   TMIN(TSDB_EP_LEN - 1, ipLen));
  strncpy(userStr, user, TMIN(TSDB_USER_LEN - 1, userLen));
  strncpy(passStr, pass, TMIN(TSDB_PASSWORD_LEN - 1, passLen));
  strncpy(dbStr,   db,   TMIN(TSDB_DB_NAME_LEN - 1, dbLen));
  return taos_connect(ipStr, userStr, passStr, dbStr, port);
}

void* doFetchRow(SRequestObj* pRequest) {
  assert(pRequest != NULL);
  SReqResultInfo* pResultInfo = &pRequest->body.resInfo;

  SEpSet epSet = {0};

  if (pResultInfo->pData == NULL || pResultInfo->current >= pResultInfo->numOfRows) {
    if (pRequest->type == TDMT_VND_QUERY) {
      // All data has returned to App already, no need to try again
      if (pResultInfo->completed) {
        return NULL;
      }

      int32_t code = scheduleFetchRows(pRequest->body.pQueryJob, (void **)&pRequest->body.resInfo.pData);
      if (code != TSDB_CODE_SUCCESS) {
        pRequest->code = code;
        return NULL;
      }

      setQueryResultByRsp(&pRequest->body.resInfo, (SRetrieveTableRsp*)pRequest->body.resInfo.pData);
      if (pResultInfo->numOfRows == 0) {
        return NULL;
      }

      goto _return;
    } else if (pRequest->type == TDMT_MND_SHOW) {
      pRequest->type = TDMT_MND_SHOW_RETRIEVE;
      epSet = getEpSet_s(&pRequest->pTscObj->pAppInfo->mgmtEp);
    } else if (pRequest->type == TDMT_VND_SHOW_TABLES) {
      pRequest->type = TDMT_VND_SHOW_TABLES_FETCH;
      SShowReqInfo* pShowReqInfo = &pRequest->body.showInfo;
      SVgroupInfo* pVgroupInfo = taosArrayGet(pShowReqInfo->pArray, pShowReqInfo->currentIndex);

      epSet.numOfEps = pVgroupInfo->numOfEps;
      epSet.inUse = pVgroupInfo->inUse;

      for (int32_t i = 0; i < epSet.numOfEps; ++i) {
        strncpy(epSet.fqdn[i], pVgroupInfo->epAddr[i].fqdn, tListLen(epSet.fqdn[i]));
        epSet.port[i] = pVgroupInfo->epAddr[i].port;
      }

    } else if (pRequest->type == TDMT_VND_SHOW_TABLES_FETCH) {
      pRequest->type = TDMT_VND_SHOW_TABLES;
      SShowReqInfo* pShowReqInfo = &pRequest->body.showInfo;
      pShowReqInfo->currentIndex += 1;
      if (pShowReqInfo->currentIndex >= taosArrayGetSize(pShowReqInfo->pArray)) {
        return NULL;
      }

      SVgroupInfo* pVgroupInfo = taosArrayGet(pShowReqInfo->pArray, pShowReqInfo->currentIndex);
      SVShowTablesReq* pShowReq = calloc(1, sizeof(SVShowTablesReq));
      pShowReq->head.vgId = htonl(pVgroupInfo->vgId);

      pRequest->body.requestMsg.len = sizeof(SVShowTablesReq);
      pRequest->body.requestMsg.pData = pShowReq;

      SMsgSendInfo* body = buildMsgInfoImpl(pRequest);

      epSet.numOfEps = pVgroupInfo->numOfEps;
      epSet.inUse = pVgroupInfo->inUse;

      for (int32_t i = 0; i < epSet.numOfEps; ++i) {
        strncpy(epSet.fqdn[i], pVgroupInfo->epAddr[i].fqdn, tListLen(epSet.fqdn[i]));
        epSet.port[i] = pVgroupInfo->epAddr[i].port;
      }

      int64_t  transporterId = 0;
      STscObj *pTscObj = pRequest->pTscObj;
      asyncSendMsgToServer(pTscObj->pAppInfo->pTransporter, &epSet, &transporterId, body);
      tsem_wait(&pRequest->body.rspSem);

      pRequest->type = TDMT_VND_SHOW_TABLES_FETCH;
    } else if (pRequest->type == TDMT_MND_SHOW_RETRIEVE && pResultInfo->pData != NULL) {
      return NULL;
    }

    SMsgSendInfo* body = buildMsgInfoImpl(pRequest);

    int64_t  transporterId = 0;
    STscObj *pTscObj = pRequest->pTscObj;
    asyncSendMsgToServer(pTscObj->pAppInfo->pTransporter, &epSet, &transporterId, body);

    tsem_wait(&pRequest->body.rspSem);

    pResultInfo->current = 0;
    if (pResultInfo->numOfRows <= pResultInfo->current) {
      return NULL;
    }
  }

_return:

  for(int32_t i = 0; i < pResultInfo->numOfCols; ++i) {
    pResultInfo->row[i] = pResultInfo->pCol[i] + pResultInfo->fields[i].bytes * pResultInfo->current;
    if (IS_VAR_DATA_TYPE(pResultInfo->fields[i].type)) {
      pResultInfo->length[i] = varDataLen(pResultInfo->row[i]);
      pResultInfo->row[i] = varDataVal(pResultInfo->row[i]);
    }
  }

  pResultInfo->current += 1;
  return pResultInfo->row;
}

static void doPrepareResPtr(SReqResultInfo* pResInfo) {
  if (pResInfo->row == NULL) {
    pResInfo->row    = calloc(pResInfo->numOfCols, POINTER_BYTES);
    pResInfo->pCol   = calloc(pResInfo->numOfCols, POINTER_BYTES);
    pResInfo->length = calloc(pResInfo->numOfCols, sizeof(int32_t));
  }
}

void setResultDataPtr(SReqResultInfo* pResultInfo, TAOS_FIELD* pFields, int32_t numOfCols, int32_t numOfRows) {
  assert(numOfCols > 0 && pFields != NULL && pResultInfo != NULL);
  if (numOfRows == 0) {
    return;
  }

  // todo check for the failure of malloc
  doPrepareResPtr(pResultInfo);

  int32_t offset = 0;
  for (int32_t i = 0; i < numOfCols; ++i) {
    pResultInfo->length[i] = pResultInfo->fields[i].bytes;
    pResultInfo->row[i]    = (char*) (pResultInfo->pData + offset * pResultInfo->numOfRows);
    pResultInfo->pCol[i]   = pResultInfo->row[i];
    offset += pResultInfo->fields[i].bytes;
  }
}

char* getConnectionDB(STscObj* pObj) {
  char *p = NULL;
  pthread_mutex_lock(&pObj->mutex);
  size_t len = strlen(pObj->db);
  if (len > 0) {
    p = strndup(pObj->db, tListLen(pObj->db));
  }

  pthread_mutex_unlock(&pObj->mutex);
  return p;
}

void setConnectionDB(STscObj* pTscObj, const char* db) {
  assert(db != NULL && pTscObj != NULL);
  pthread_mutex_lock(&pTscObj->mutex);
  tstrncpy(pTscObj->db, db, tListLen(pTscObj->db));
  pthread_mutex_unlock(&pTscObj->mutex);
}

void setQueryResultByRsp(SReqResultInfo* pResultInfo, const SRetrieveTableRsp* pRsp) {
  assert(pResultInfo != NULL && pRsp != NULL);

  pResultInfo->pRspMsg   = (const char*) pRsp;
  pResultInfo->pData     = (void*) pRsp->data;
  pResultInfo->numOfRows = htonl(pRsp->numOfRows);
  pResultInfo->current   = 0;
  pResultInfo->completed = (pRsp->completed == 1);

  setResultDataPtr(pResultInfo, pResultInfo->fields, pResultInfo->numOfCols, pResultInfo->numOfRows);
}