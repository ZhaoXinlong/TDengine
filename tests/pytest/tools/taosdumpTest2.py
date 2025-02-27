###################################################################
#           Copyright (c) 2016 by TAOS Technologies, Inc.
#                     All rights reserved.
#
#  This file is proprietary and confidential to TAOS Technologies.
#  No part of this file may be reproduced, stored, transmitted,
#  disclosed or used in any form or by any means other than as
#  expressly provided by the written permission from Jianhui Tao
#
###################################################################

# -*- coding: utf-8 -*-

from logging.config import dictConfig
import sys
import os
from util.log import *
from util.cases import *
from util.sql import *
from util.dnodes import *
import string
import random


class TDTestCase:    

    def init(self, conn, logSql):
        tdLog.debug("start to execute %s" % __file__)
        tdSql.init(conn.cursor(), logSql)

        self.ts = 1601481600000
        self.numberOfTables = 1
        self.numberOfRecords = 15000

    def getBuildPath(self):
        selfPath = os.path.dirname(os.path.realpath(__file__))

        if ("community" in selfPath):
            projPath = selfPath[:selfPath.find("community")]
        else:
            projPath = selfPath[:selfPath.find("tests")]

        for root, dirs, files in os.walk(projPath):
            if ("taosd" in files):
                rootRealPath = os.path.dirname(os.path.realpath(root))
                if ("packaging" not in rootRealPath):
                    buildPath = root[:len(root) - len("/build/bin")]
                    break
        return buildPath

    def generateString(self, length):
        chars = string.ascii_uppercase + string.ascii_lowercase
        v = ""
        for i in range(length):
            v += random.choice(chars)
        return v

    def run(self):
        tdSql.prepare()

        tdSql.execute("create table st(ts timestamp, c1 timestamp, c2 int, c3 bigint, c4 float, c5 double, c6 binary(8), c7 smallint, c8 tinyint, c9 bool, c10 nchar(8)) tags(t1 int)")
        tdSql.execute("create table t1 using st tags(0)")
        currts = self.ts
        finish = 0
        while(finish < self.numberOfRecords):
            sql = "insert into t1 values"
            for i in range(finish, self.numberOfRecords):
                sql += "(%d, 1019774612, 29931, 1442173978, 165092.468750, 1128.643179, 'MOCq1pTu', 18405, 82, 0, 'g0A6S0Fu')" % (currts + i)
                finish = i + 1
                if (1048576 - len(sql)) < 16384:
                    break
            tdSql.execute(sql)

        buildPath = self.getBuildPath()
        if (buildPath == ""):
            tdLog.exit("taosdump not found!")
        else:
            tdLog.info("taosdump found in %s" % buildPath)
        binPath = buildPath + "/build/bin/"

        os.system("rm /tmp/*.sql")
        os.system("rm /tmp/*.avro*")
        os.system(
            "%staosdump --databases db -o /tmp " %
            binPath)

        tdSql.execute("drop database db")
        tdSql.query("show databases")
        tdSql.checkRows(0)

        os.system("%staosdump -i /tmp -y" % binPath)

        tdSql.query("show databases")
        tdSql.checkRows(1)
        tdSql.checkData(0, 0, 'db')

        tdSql.execute("use db")
        tdSql.query("show stables")
        tdSql.checkRows(1)
        tdSql.checkData(0, 0, 'st')

        tdSql.query("select count(*) from t1")
        tdSql.checkData(0, 0, self.numberOfRecords)        

        # test case for TS-1225
        tdSql.execute("create database test")
        tdSql.execute("use test")
        tdSql.execute("create table stb(ts timestamp, c1 binary(16374), c2 binary(16374), c3 binary(16374)) tags(t1 nchar(256))")
        tdSql.execute("insert into t1 using stb tags('t1') values(now, '%s', '%s', '%s')" % (self.generateString(16374), self.generateString(16374), self.generateString(16374)))
        
        os.system("rm /tmp/*.sql")
        os.system("rm /tmp/*.avro*")
        os.system("%staosdump -D test -o /tmp -y" % binPath)

        tdSql.execute("drop database test")
        tdSql.query("show databases")
        tdSql.checkRows(1)

        os.system("%staosdump -i /tmp -y" % binPath)

        tdSql.execute("use test")
        tdSql.error("show vnodes '' ")
        tdSql.query("show stables")
        tdSql.checkRows(1)
        tdSql.checkData(0, 0, 'stb')

        tdSql.query("select * from stb")
        tdSql.checkRows(1)
        os.system("rm -rf dump_result.txt")

    def stop(self):
        tdSql.close()
        tdLog.success("%s successfully executed" % __file__)


tdCases.addWindows(__file__, TDTestCase())
tdCases.addLinux(__file__, TDTestCase())
