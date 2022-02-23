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

#include "tlinearhash.h"
#include "tcfg.h"
#include "taoserror.h"
#include "tpagedbuf.h"

#define LHASH_CAP_RATIO 0.85

// Always located in memory
typedef struct SLHashBucket {
  SArray        *pPageIdList;
  int32_t        size;            // the number of element in this entry
} SLHashBucket;

typedef struct SLHashObj {
  SDiskbasedBuf *pBuf;
  _hash_fn_t     hashFn;
  int32_t        tuplesPerPage;
  SLHashBucket **pBucket;         // entry list
  int32_t        numOfAlloc;      // number of allocated bucket ptr slot
  int32_t        bits;            // the number of bits used in hash
  int32_t        numOfBuckets;    // the number of buckets
  int64_t        size;            // the number of total items
} SLHashObj;

/**
 * the data struct for each hash node
 * +-----------+-------+--------+
 * | SLHashNode|  key  |  data  |
 * +-----------+-------+--------+
 */
typedef struct SLHashNode {
  int32_t   keyLen;
  int32_t   dataLen;
} SLHashNode;

#define GET_LHASH_NODE_KEY(_n)      (((char*)(_n)) + sizeof(SLHashNode))
#define GET_LHASH_NODE_DATA(_n)     ((char*)(_n) + sizeof(SLHashNode) + (_n)->keyLen)
#define GET_LHASH_NODE_LEN(_n)      (sizeof(SLHashNode) + ((SLHashNode*)(_n))->keyLen + ((SLHashNode*)(_n))->dataLen)

static int32_t doAddNewBucket(SLHashObj* pHashObj);

static int32_t doGetBucketIdFromHashVal(int32_t hashv, int32_t bits) {
  return hashv & ((1ul << (bits)) - 1);
}

static int32_t doGetAlternativeBucketId(int32_t bucketId, int32_t bits, int32_t numOfBuckets) {
  int32_t v = bucketId - (1ul << (bits - 1));

  ASSERT(v < numOfBuckets);
  return v;
}

SLHashObj* tHashInit(int32_t inMemPages, int32_t pageSize, _hash_fn_t fn, int32_t numOfTuplePerPage) {
  SLHashObj* pHashObj = calloc(1, sizeof(SLHashObj));
  if (pHashObj == NULL) {
    terrno = TSDB_CODE_OUT_OF_MEMORY;
    return NULL;
  }

  int32_t code = createDiskbasedBuf(&pHashObj->pBuf, pageSize, inMemPages * pageSize, 0, "/tmp");
  if (code != 0) {
    terrno = code;
    return NULL;
  }

  /**
   * The number of bits in the hash value, which is used to decide the exact bucket where the object should be located in.
   * The initial value is 0.
   */
  pHashObj->bits   = 0;
  pHashObj->hashFn = fn;
  pHashObj->tuplesPerPage = numOfTuplePerPage;

  pHashObj->numOfAlloc   = 4;  // initial allocated array list
  pHashObj->pBucket = calloc(pHashObj->numOfAlloc, POINTER_BYTES);

  code = doAddNewBucket(pHashObj);
  if (code != TSDB_CODE_SUCCESS) {
    destroyDiskbasedBuf(pHashObj->pBuf);
    tfree(pHashObj);
    terrno = code;
    return NULL;
  }

  return pHashObj;
}

void* tHashCleanup(SLHashObj* pHashObj) {
  destroyDiskbasedBuf(pHashObj->pBuf);
  for(int32_t i = 0; i < pHashObj->numOfBuckets; ++i) {
    taosArrayDestroy(pHashObj->pBucket[i]->pPageIdList);
    tfree(pHashObj->pBucket[i]);
  }

  tfree(pHashObj->pBucket);
  tfree(pHashObj);
  return NULL;
}

static void doCopyObject(char* p, const void* key, int32_t keyLen, const void* data, int32_t size) {
  *(int32_t*) p = keyLen;
  p += sizeof(int32_t);
  *(int32_t*) p = size;
  p += sizeof(int32_t);

  memcpy(p, key, keyLen);
  p += keyLen;

  memcpy(p, data, size);
}

static int32_t doAddToBucket(SLHashObj* pHashObj, SLHashBucket* pBucket, int32_t index, const void* key, int32_t keyLen,
                             const void* data, int32_t size) {
  int32_t pageId = *(int32_t*)taosArrayGetLast(pBucket->pPageIdList);

  SFilePage* pPage = getBufPage(pHashObj->pBuf, pageId);
  ASSERT (pPage != NULL);

  // put to current buf page
  size_t nodeSize = sizeof(SLHashNode) + keyLen + size;
  ASSERT(nodeSize <= getBufPageSize(pHashObj->pBuf));

  if (pPage->num + nodeSize > getBufPageSize(pHashObj->pBuf)) {
    releaseBufPage(pHashObj->pBuf, pPage);

    // allocate the overflow buffer page to hold this k/v.
    int32_t newPageId = -1;
    SFilePage* pNewPage = getNewBufPage(pHashObj->pBuf, 0, &newPageId);
    if (pNewPage == 0) {
      // TODO handle error
    }

    taosArrayPush(pBucket->pPageIdList, &newPageId);

    doCopyObject(pNewPage->data, key, keyLen, data, size);
    pNewPage->num = nodeSize;

    setBufPageDirty(pNewPage, true);
    releaseBufPage(pHashObj->pBuf, pNewPage);
  } else {
    char* p = pPage->data + pPage->num;
    doCopyObject(p, key, keyLen, data, size);
    pPage->num += nodeSize;
    setBufPageDirty(pPage, true);
    releaseBufPage(pHashObj->pBuf, pPage);
  }

  pBucket->size += 1;
//  printf("===> add to bucket:0x%x, num:%d, key:%d\n", index, pBucket->size, *(int*) key);
}

// TODO merge the fragments on multiple pages to recycle the empty disk page ASAP
static void doRemoveFromBucket(SFilePage* pPage, SLHashNode* pNode, SLHashBucket* pBucket) {
  ASSERT(pPage != NULL && pNode != NULL);

  int32_t len = GET_LHASH_NODE_LEN(pNode);
  char* p = (char*) pNode + len;

  char* pEnd = pPage->data + pPage->num;
  memmove(pNode, p, (pEnd - p));

  pPage->num -= len;
  if (pPage->num == 0) {
    // this page is empty, could be recycle in the future.
  }

  setBufPageDirty(pPage, true);
  pBucket->size -= 1;
}

static int32_t doAddNewBucket(SLHashObj* pHashObj) {
  if (pHashObj->numOfBuckets + 1 > pHashObj->numOfAlloc) {
    int32_t newLen = pHashObj->numOfAlloc * 1.25;
    if (newLen == pHashObj->numOfAlloc) {
      newLen += 4;
    }

    char* p = realloc(pHashObj->pBucket, POINTER_BYTES * newLen);
    if (p == NULL) {
      return TSDB_CODE_OUT_OF_MEMORY;
    }

    memset(p + POINTER_BYTES * pHashObj->numOfBuckets, 0, newLen - pHashObj->numOfBuckets);
    pHashObj->pBucket = (SLHashBucket**) p;
    pHashObj->numOfAlloc = newLen;
  }

  SLHashBucket* pBucket = calloc(1, sizeof(SLHashBucket));
  pHashObj->pBucket[pHashObj->numOfBuckets] = pBucket;

  pBucket->pPageIdList = taosArrayInit(2, sizeof(int32_t));
  if (pBucket->pPageIdList == NULL || pBucket == NULL) {
    return TSDB_CODE_OUT_OF_MEMORY;
  }

  int32_t pageId = -1;
  SFilePage* p = getNewBufPage(pHashObj->pBuf, 0, &pageId);
  releaseBufPage(pHashObj->pBuf, p);

  taosArrayPush(pBucket->pPageIdList, &pageId);

  pHashObj->numOfBuckets += 1;
//  printf("---------------add new bucket, id:0x%x, total:%d\n", pHashObj->numOfBuckets - 1, pHashObj->numOfBuckets);
  return TSDB_CODE_SUCCESS;
}

int32_t tHashPut(SLHashObj* pHashObj, const void *key, size_t keyLen, void *data, size_t size) {
  ASSERT(pHashObj != NULL && key != NULL);

  if (pHashObj->bits == 0) {
    SLHashBucket* pBucket = pHashObj->pBucket[0];
    doAddToBucket(pHashObj, pBucket, 0, key, keyLen, data, size);
  } else {
    int32_t hashVal = pHashObj->hashFn(key, keyLen);
    int32_t v = doGetBucketIdFromHashVal(hashVal, pHashObj->bits);

    if (pHashObj->numOfBuckets > v) {
      SLHashBucket* pBucket = pHashObj->pBucket[v];

      // TODO check return code
      doAddToBucket(pHashObj, pBucket, v, key, keyLen, data, size);
    } else {  // no matched bucket exists, find the candidate bucket
      int32_t bucketId = doGetAlternativeBucketId(v, pHashObj->bits, pHashObj->numOfBuckets);
//      printf("bucketId: 0x%x not exists, put it into 0x%x instead\n", v, bucketId);

      SLHashBucket* pBucket = pHashObj->pBucket[bucketId];
      doAddToBucket(pHashObj, pBucket, bucketId, key, keyLen, data, size);
    }
  }

  pHashObj->size += 1;

  // Too many records, needs to bucket split
  if ((pHashObj->numOfBuckets * LHASH_CAP_RATIO * pHashObj->tuplesPerPage) < pHashObj->size) {
    int32_t newBucketId = pHashObj->numOfBuckets;

    int32_t code = doAddNewBucket(pHashObj);
    int32_t numOfBits = ceil(log(pHashObj->numOfBuckets) / log(2));
    if (numOfBits > pHashObj->bits) {
//      printf("extend the bits from %d to %d, new bucket:%d\n", pHashObj->bits, numOfBits, newBucketId);

      ASSERT(numOfBits == pHashObj->bits + 1);
      pHashObj->bits = numOfBits;
    }

    int32_t splitBucketId = (1ul << (pHashObj->bits - 1)) ^ newBucketId;

    // load all data in this bucket and check if the data needs to relocated into the new bucket
    SLHashBucket* pBucket = pHashObj->pBucket[splitBucketId];
//    printf("split %d items' bucket:0x%x to new bucket:0x%x\n", pBucket->size, splitBucketId, newBucketId);

    for (int32_t i = 0; i < taosArrayGetSize(pBucket->pPageIdList); ++i) {
      int32_t    pageId = *(int32_t*)taosArrayGet(pBucket->pPageIdList, i);
      SFilePage* p = getBufPage(pHashObj->pBuf, pageId);

      char* pStart = p->data;
      while (pStart - p->data < p->num) {
        SLHashNode* pNode = (SLHashNode*)pStart;

        char* k = GET_LHASH_NODE_KEY(pNode);
        int32_t hashv = pHashObj->hashFn(k, pNode->keyLen);

        int32_t v1 = hashv & ((1ul << (pHashObj->bits)) - 1);
        if (v1 != splitBucketId) {  // place it into the new bucket
          ASSERT(v1 == newBucketId);
//          printf("move key:%d to 0x%x bucket, remain items:%d\n", *(int32_t*)k, v1, pBucket->size - 1);

          SLHashBucket* pNewBucket = pHashObj->pBucket[newBucketId];
          doAddToBucket(pHashObj, pNewBucket, newBucketId, (void*)GET_LHASH_NODE_KEY(pNode), pNode->keyLen,
                        GET_LHASH_NODE_KEY(pNode), pNode->dataLen);
          doRemoveFromBucket(p, pNode, pBucket);
        } else {
//          printf("check key:%d, located into: %d, skip it\n", *(int*) k, v1);

          int32_t nodeSize = GET_LHASH_NODE_LEN(pStart);
          pStart += nodeSize;
        }
      }
      releaseBufPage(pHashObj->pBuf, p);
    }
  }
}

char* tHashGet(SLHashObj* pHashObj, const void *key, size_t keyLen) {
  ASSERT(pHashObj != NULL && key != NULL && keyLen > 0);
  int32_t hashv = pHashObj->hashFn(key, keyLen);

  int32_t bucketId = doGetBucketIdFromHashVal(hashv, pHashObj->bits);
  if (bucketId >= pHashObj->numOfBuckets) {
    bucketId = doGetAlternativeBucketId(bucketId, pHashObj->bits, pHashObj->numOfBuckets);
  }

  SLHashBucket* pBucket = pHashObj->pBucket[bucketId];
  for (int32_t i = 0; i < taosArrayGetSize(pBucket->pPageIdList); ++i) {
    int32_t    pageId = *(int32_t*)taosArrayGet(pBucket->pPageIdList, i);
    SFilePage* p = getBufPage(pHashObj->pBuf, pageId);

    char* pStart = p->data;
    while (pStart - p->data < p->num) {
      SLHashNode* pNode = (SLHashNode*)pStart;

      char* k = GET_LHASH_NODE_KEY(pNode);
      if (pNode->keyLen == keyLen && (memcmp(key, k, keyLen) == 0)) {
        releaseBufPage(pHashObj->pBuf, p);
        return GET_LHASH_NODE_DATA(pNode);
      } else {
        pStart += GET_LHASH_NODE_LEN(pStart);
      }
    }

    releaseBufPage(pHashObj->pBuf, p);
  }

  return NULL;
}

int32_t tHashRemove(SLHashObj* pHashObj, const void *key, size_t keyLen) {

}

void tHashPrint(const SLHashObj* pHashObj, int32_t type) {
  printf("==================== linear hash ====================\n");
  printf("total bucket:%d, size:%ld, ratio:%.2f\n", pHashObj->numOfBuckets, pHashObj->size, LHASH_CAP_RATIO);

  dBufSetPrintInfo(pHashObj->pBuf);

  if (type == LINEAR_HASH_DATA) {
    for (int32_t i = 0; i < pHashObj->numOfBuckets; ++i) {
//      printf("bucket: 0x%x, obj:%d, page:%d\n", i, pHashObj->pBucket[i]->size,
//             (int)taosArrayGetSize(pHashObj->pBucket[i]->pPageIdList));
    }
  } else {
    dBufPrintStatis(pHashObj->pBuf);
  }
}