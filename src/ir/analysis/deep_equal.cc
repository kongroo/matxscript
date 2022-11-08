// Copyright 2022 ByteDance Ltd. and/or its affiliates.
/*
 * Acknowledgement: This file originates from incubator-tvm
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/*!
 * \file ir/analysis/deep_equal.cc
 * \brief Deep equality checking.
 */
#include <matxscript/ir/analysis.h>

#include <matxscript/ir/_base/reflection.h>
#include <matxscript/ir/_base/structural_equal.h>
#include <matxscript/runtime/registry.h>

namespace matxscript {
namespace ir {

class DeepCmpSEqualHandler : public SEqualReducer::Handler {
 public:
  // use direct recursion.
  bool SEqualReduce(const ObjectRef& lhs, const ObjectRef& rhs, bool map_free_vars) final {
    if (lhs.same_as(rhs))
      return true;
    if (!lhs.defined() && rhs.defined())
      return false;
    if (!rhs.defined() && lhs.defined())
      return false;
    if (lhs->type_index() != rhs->type_index())
      return false;
    return vtable_->SEqualReduce(lhs.get(), rhs.get(), SEqualReducer(this, false));
  }

  ObjectRef MapLhsToRhs(const ObjectRef& lhs) final {
    return ObjectRef(nullptr);
  }

  void MarkGraphNode() final {
  }

 private:
  // reflection vtable
  runtime::ReflectionVTable* vtable_ = runtime::ReflectionVTable::Global();
};

bool ExprDeepEqual::operator()(const PrimExpr& lhs, const PrimExpr& rhs) const {
  // quick path
  if (lhs.same_as(rhs))
    return true;
  if (!lhs.defined() && rhs.defined())
    return false;
  if (!rhs.defined() && lhs.defined())
    return false;
  if (lhs->type_index() != rhs->type_index())
    return false;
  if (auto* plhs = lhs.as<IntImmNode>()) {
    auto* prhs = rhs.as<IntImmNode>();
    return plhs->dtype == prhs->dtype && plhs->value == prhs->value;
  }
  return DeepCmpSEqualHandler().SEqualReduce(lhs, rhs, false);
}

MATXSCRIPT_REGISTER_GLOBAL("ir.analysis.expr_deep_equal")
    .set_body_typed([](const PrimExpr& lhs, const PrimExpr& rhs) {
      return ExprDeepEqual()(lhs, rhs);
    });

}  // namespace ir
}  // namespace matxscript