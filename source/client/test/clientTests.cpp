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

#include <gtest/gtest.h>
#include <taoserror.h>
#include <iostream>
#pragma GCC diagnostic ignored "-Wwrite-strings"

#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wsign-compare"

#include "taos.h"
#include "tglobal.h"
#include "../inc/clientInt.h"

namespace {
}  // namespace

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

TEST(testCase, driverInit_Test) {
  taos_init();
}

TEST(testCase, connect_Test) {
  TAOS* pConn = taos_connect("ubuntu", "root", "taosdata", NULL, 0);
//  assert(pConn != NULL);
  taos_close(pConn);
}

TEST(testCase, create_user_Test) {
  TAOS* pConn = taos_connect("ubuntu", "root", "taosdata", NULL, 0);
//  assert(pConn != NULL);

  TAOS_RES* pRes = taos_query(pConn, "create user abc pass 'abc'");
  if (taos_errno(pRes) != TSDB_CODE_SUCCESS) {
    printf("failed to create user, reason:%s\n", taos_errstr(pRes));
  }

  taos_free_result(pRes);
  taos_close(pConn);
}

TEST(testCase, create_account_Test) {
  TAOS* pConn = taos_connect("ubuntu", "root", "taosdata", NULL, 0);
  assert(pConn != NULL);

  TAOS_RES* pRes = taos_query(pConn, "create account aabc pass 'abc'");
  if (taos_errno(pRes) != TSDB_CODE_SUCCESS) {
    printf("failed to create user, reason:%s\n", taos_errstr(pRes));
  }

  taos_free_result(pRes);
  taos_close(pConn);
}

TEST(testCase, drop_account_Test) {
  TAOS* pConn = taos_connect("ubuntu", "root", "taosdata", NULL, 0);
  assert(pConn != NULL);

  TAOS_RES* pRes = taos_query(pConn, "drop account aabc");
  if (taos_errno(pRes) != TSDB_CODE_SUCCESS) {
    printf("failed to create user, reason:%s\n", taos_errstr(pRes));
  }

  taos_free_result(pRes);
  taos_close(pConn);
}

TEST(testCase, show_user_Test) {
  TAOS* pConn = taos_connect("ubuntu", "root", "taosdata", NULL, 0);
//  assert(pConn != NULL);

  TAOS_RES* pRes = taos_query(pConn, "show users");
  TAOS_ROW pRow = NULL;

  TAOS_FIELD* pFields = taos_fetch_fields(pRes);
  int32_t numOfFields = taos_num_fields(pRes);

  char str[512] = {0};
  while((pRow = taos_fetch_row(pRes)) != NULL) {
    int32_t code = taos_print_row(str, pRow, pFields, numOfFields);
    printf("%s\n", str);
  }

  taos_close(pConn);
}

TEST(testCase, drop_user_Test) {
  TAOS* pConn = taos_connect("ubuntu", "root", "taosdata", NULL, 0);
  assert(pConn != NULL);

  TAOS_RES* pRes = taos_query(pConn, "drop user abc");
  if (taos_errno(pRes) != TSDB_CODE_SUCCESS) {
    printf("failed to create user, reason:%s\n", taos_errstr(pRes));
  }

  taos_free_result(pRes);
  taos_close(pConn);
}

TEST(testCase, show_db_Test) {
  TAOS* pConn = taos_connect("ubuntu", "root", "taosdata", NULL, 0);
//  assert(pConn != NULL);

  TAOS_RES* pRes = taos_query(pConn, "show databases");
  TAOS_ROW pRow = NULL;

  TAOS_FIELD* pFields = taos_fetch_fields(pRes);
  int32_t numOfFields = taos_num_fields(pRes);

  char str[512] = {0};
  while((pRow = taos_fetch_row(pRes)) != NULL) {
    int32_t code = taos_print_row(str, pRow, pFields, numOfFields);
    printf("%s\n", str);
  }

  taos_close(pConn);
}

TEST(testCase, create_db_Test) {
  TAOS* pConn = taos_connect("ubuntu", "root", "taosdata", NULL, 0);
  assert(pConn != NULL);

  TAOS_RES* pRes = taos_query(pConn, "create database abc1");

  TAOS_FIELD* pFields = taos_fetch_fields(pRes);
  ASSERT_TRUE(pFields == NULL);

  int32_t numOfFields = taos_num_fields(pRes);
  ASSERT_EQ(numOfFields, 0);

  taos_close(pConn);
}

TEST(testCase, use_db_test) {
  TAOS* pConn = taos_connect("ubuntu", "root", "taosdata", NULL, 0);
  assert(pConn != NULL);

  TAOS_RES* pRes = taos_query(pConn, "use abc1");

  TAOS_FIELD* pFields = taos_fetch_fields(pRes);
  ASSERT_TRUE(pFields == NULL);

  int32_t numOfFields = taos_num_fields(pRes);
  ASSERT_EQ(numOfFields, 0);

  taos_close(pConn);
}

TEST(testCase, create_stable_Test) {
  TAOS* pConn = taos_connect("ubuntu", "root", "taosdata", NULL, 0);
  assert(pConn != NULL);

  TAOS_RES* pRes = taos_query(pConn, "use abc1");
  taos_free_result(pRes);

  pRes = taos_query(pConn, "create stable st1(ts timestamp, k int) tags(a int)");

  TAOS_FIELD* pFields = taos_fetch_fields(pRes);
  ASSERT_TRUE(pFields == NULL);

  int32_t numOfFields = taos_num_fields(pRes);
  ASSERT_EQ(numOfFields, 0);

  taos_close(pConn);
}