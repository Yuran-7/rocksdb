// Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once
#include "rocksdb/db.h"

namespace ROCKSDB_NAMESPACE {
/*
验证单个 SST 文件的完整性
通过校验和检测文件损坏
用于数据一致性检查和故障诊断
*/
Status VerifySstFileChecksumInternal(const Options& options,
                                     const EnvOptions& env_options,
                                     const ReadOptions& read_options,
                                     const std::string& file_path,
                                     const SequenceNumber& largest_seqno = 0);
}  // namespace ROCKSDB_NAMESPACE
