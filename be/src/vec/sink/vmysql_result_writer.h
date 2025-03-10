// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#pragma once
#include "runtime/primitive_type.h"
#include "util/mysql_row_buffer.h"
#include "util/runtime_profile.h"
#include "vec/core/block.h"
#include "vec/sink/vresult_writer.h"

namespace doris {
class BufferControlBlock;
class TFetchDataResult;

namespace vectorized {
class VExprContext;

template <bool is_binary_format = false>
class VMysqlResultWriter final : public VResultWriter {
public:
    using ResultList = std::vector<std::unique_ptr<TFetchDataResult>>;

    VMysqlResultWriter(BufferControlBlock* sinker,
                       const std::vector<vectorized::VExprContext*>& output_vexpr_ctxs,
                       RuntimeProfile* parent_profile);

    virtual Status init(RuntimeState* state) override;

    virtual Status append_block(Block& block) override;

    virtual bool can_sink() override;

    virtual Status close() override;

    const ResultList& results() { return _results; }

private:
    void _init_profile();

    template <PrimitiveType type, bool is_nullable>
    Status _add_one_column(const ColumnPtr& column_ptr, std::unique_ptr<TFetchDataResult>& result,
                           std::vector<MysqlRowBuffer<is_binary_format>>& rows_buffer,
                           int scale = -1, const DataTypes& sub_types = DataTypes());
    int _add_one_cell(const ColumnPtr& column_ptr, size_t row_idx, const DataTypePtr& type,
                      MysqlRowBuffer<is_binary_format>& buffer, int scale = -1);

private:
    BufferControlBlock* _sinker;

    const std::vector<vectorized::VExprContext*>& _output_vexpr_ctxs;

    RuntimeProfile* _parent_profile; // parent profile from result sink. not owned
    // total time cost on append batch operation
    RuntimeProfile::Counter* _append_row_batch_timer = nullptr;
    // tuple convert timer, child timer of _append_row_batch_timer
    RuntimeProfile::Counter* _convert_tuple_timer = nullptr;
    // file write timer, child timer of _append_row_batch_timer
    RuntimeProfile::Counter* _result_send_timer = nullptr;
    // number of sent rows
    RuntimeProfile::Counter* _sent_rows_counter = nullptr;
    // for synchronized results
    ResultList _results;
    // If true, no block will be sent
    bool _is_dry_run = false;
};
} // namespace vectorized
} // namespace doris
