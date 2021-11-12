/*
 * Copyright (c) 2019 TAOS Data, Inc. <cli@taosdata.com>
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

#ifndef _TD_LIBS_SYNC_TYPE_H
#define _TD_LIBS_SYNC_TYPE_H

#include <stdint.h>
#include "sync.h"
#include "osMath.h"

#define SYNC_NON_NODE_ID -1
#define SYNC_NON_TERM     0

typedef int32_t SyncTime;
typedef uint32_t SyncTick;

typedef struct SSyncRaft SSyncRaft;

typedef struct SSyncRaftProgress SSyncRaftProgress;
typedef struct SSyncRaftProgressMap SSyncRaftProgressMap;
typedef struct SSyncRaftProgressTrackerConfig SSyncRaftProgressTrackerConfig;

typedef struct SSyncRaftProgressTracker SSyncRaftProgressTracker;

typedef struct SSyncRaftChanger SSyncRaftChanger;

typedef struct SSyncRaftLog SSyncRaftLog;

typedef struct SSyncRaftEntry SSyncRaftEntry;

#if 0
#ifndef MIN
#define MIN(x, y) (((x) < (y)) ? (x) : (y))
#endif

#ifndef MAX
#define MAX(x, y) (((x) > (y)) ? (x) : (y))
#endif
#endif

typedef struct {
  int32_t   replica;
  SyncNodeId nodeId[TSDB_MAX_REPLICA];
} SSyncRaftNodeMap;

typedef enum {
  SYNC_RAFT_CAMPAIGN_PRE_ELECTION = 0,
  SYNC_RAFT_CAMPAIGN_ELECTION     = 1,
  SYNC_RAFT_CAMPAIGN_TRANSFER     = 2,
} ESyncRaftElectionType;

typedef enum {
  // the init vote resp status
  SYNC_RAFT_VOTE_RESP_UNKNOWN = 0,

  // grant the vote request
  SYNC_RAFT_VOTE_RESP_GRANT   = 1,

  //reject the vote request
  SYNC_RAFT_VOTE_RESP_REJECT  = 2,
} ESyncRaftVoteType;

#endif  /* _TD_LIBS_SYNC_TYPE_H */
