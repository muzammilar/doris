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

#include <gen_cpp/Types_types.h>
#include <jni.h>
#include <stddef.h>
#include <stdint.h>

#include <functional>
#include <memory>
#include <mutex>
#include <ostream>

#include "common/logging.h"
#include "common/status.h"
#include "udf/udf.h"
#include "util/jni-util.h"
#include "vec/core/block.h"
#include "vec/core/column_numbers.h"
#include "vec/core/column_with_type_and_name.h"
#include "vec/core/columns_with_type_and_name.h"
#include "vec/core/types.h"
#include "vec/data_types/data_type.h"
#include "vec/functions/function.h"

namespace doris::vectorized {

class JavaUdfPreparedFunction : public PreparedFunctionImpl {
public:
    using execute_call_back = std::function<Status(FunctionContext* context, Block& block,
                                                   const ColumnNumbers& arguments, uint32_t result,
                                                   size_t input_rows_count)>;

    explicit JavaUdfPreparedFunction(const execute_call_back& func, const std::string& name)
            : callback_function(func), name(name) {}

    String get_name() const override { return name; }

protected:
    Status execute_impl(FunctionContext* context, Block& block, const ColumnNumbers& arguments,
                        uint32_t result, size_t input_rows_count) const override {
        return callback_function(context, block, arguments, result, input_rows_count);
    }

    bool use_default_implementation_for_nulls() const override { return false; }

private:
    execute_call_back callback_function;
    std::string name;
};

class JavaFunctionCall : public IFunctionBase {
public:
    JavaFunctionCall(const TFunction& fn, const DataTypes& argument_types,
                     const DataTypePtr& return_type);

    static FunctionBasePtr create(const TFunction& fn, const ColumnsWithTypeAndName& argument_types,
                                  const DataTypePtr& return_type) {
        DataTypes data_types(argument_types.size());
        for (size_t i = 0; i < argument_types.size(); ++i) {
            data_types[i] = argument_types[i].type;
        }
        return std::make_shared<JavaFunctionCall>(fn, data_types, return_type);
    }

    /// Get the main function name.
    String get_name() const override { return fn_.name.function_name; }

    const DataTypes& get_argument_types() const override { return _argument_types; }
    const DataTypePtr& get_return_type() const override { return _return_type; }

    PreparedFunctionPtr prepare(FunctionContext* context, const Block& sample_block,
                                const ColumnNumbers& arguments, uint32_t result) const override {
        return std::make_shared<JavaUdfPreparedFunction>(
                [this](auto&& PH1, auto&& PH2, auto&& PH3, auto&& PH4, auto&& PH5) {
                    return JavaFunctionCall::execute_impl(
                            std::forward<decltype(PH1)>(PH1), std::forward<decltype(PH2)>(PH2),
                            std::forward<decltype(PH3)>(PH3), std::forward<decltype(PH4)>(PH4),
                            std::forward<decltype(PH5)>(PH5));
                },
                fn_.name.function_name);
    }

    Status open(FunctionContext* context, FunctionContext::FunctionStateScope scope) override;

    Status execute_impl(FunctionContext* context, Block& block, const ColumnNumbers& arguments,
                        uint32_t result, size_t input_rows_count) const;

    Status close(FunctionContext* context, FunctionContext::FunctionStateScope scope) override;

    bool is_use_default_implementation_for_constants() const override { return true; }

    bool is_udf_function() const override { return true; }

private:
    const TFunction& fn_;
    const DataTypes _argument_types;
    const DataTypePtr _return_type;

    struct JniContext {
        // Do not save parent directly, because parent is in VExpr, but jni context is in FunctionContext
        // The deconstruct sequence is not determined, it will core.
        // JniContext's lifecycle should same with function context, not related with expr
        jclass executor_cl;
        jmethodID executor_ctor_id;
        jmethodID executor_evaluate_id;
        jmethodID executor_close_id;
        jobject executor = nullptr;
        bool is_closed = false;
        bool open_successes = false;

        JniContext() = default;

        Status close() {
            if (!open_successes) {
                LOG_WARNING("maybe open failed, need check the reason");
                return Status::OK(); //maybe open failed, so can't call some jni
            }
            if (is_closed) {
                return Status::OK();
            }
            VLOG_DEBUG << "Free resources for JniContext";
            JNIEnv* env = nullptr;
            Status status = JniUtil::GetJNIEnv(&env);
            if (!status.ok() || env == nullptr) {
                LOG(WARNING) << "errors while get jni env " << status;
                return status;
            }
            env->CallNonvirtualVoidMethodA(executor, executor_cl, executor_close_id, nullptr);
            RETURN_ERROR_IF_EXC(env);
            env->DeleteGlobalRef(executor);
            RETURN_ERROR_IF_EXC(env);
            env->DeleteGlobalRef(executor_cl);
            RETURN_IF_ERROR(JniUtil::GetJniExceptionMsg(env));
            is_closed = true;
            return Status::OK();
        }
    };
};

} // namespace doris::vectorized
