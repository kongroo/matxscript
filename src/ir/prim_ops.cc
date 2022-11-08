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
 * \file ir/op/prim_ops.cc
 *
 *  Common operator definitions for ops in ir/prim_ops.h
 */

// Centralized header for constant folders.
#include <matxscript/ir/prim_ops.h>
#include "./const_fold.h"

#include <cmath>

#include <matxscript/ir/_base/repr_printer.h>
#include <matxscript/ir/op_attr_types.h>
#include <matxscript/ir/prim_builtin.h>
#include <matxscript/ir/type.h>
#include <matxscript/runtime/registry.h>

namespace matxscript {
namespace ir {

using namespace runtime;

// macro to register an unary op
#define MATXSCRIPT_IR_REGISTER_PURE_UNARY_OP(OpName)                             \
  MATXSCRIPT_IR_REGISTER_OP(OpName).set_num_inputs(1).set_attr<TCallEffectKind>( \
      "TCallEffectKind", Integer(CallEffectKind::kPure))

// macro to register an binary op
#define MATXSCRIPT_IR_REGISTER_PURE_BINARY_OP(OpName)                            \
  MATXSCRIPT_IR_REGISTER_OP(OpName).set_num_inputs(2).set_attr<TCallEffectKind>( \
      "TCallEffectKind", Integer(CallEffectKind::kPure))

Type GetType(const PrimExpr& expr) {
  // TODO(tqchen): add recursive type inference for Call here
  // once we introduced the corresponding fields to the IR.
  if (auto* ptr = expr.as<PrimVarNode>()) {
    // If Var has a more refined type annotation,
    // return the type anotation
    if (ptr->type_annotation.defined()) {
      return ptr->type_annotation;
    }
  }
  // Default: return the type indicated by the dtype.
  runtime::DataType dtype = expr.dtype();
  if (dtype.is_void()) {
    return VoidType();
  }
  return PrimType(dtype);
}

// simple cast that only checks if type matches and cast
inline PrimExpr SimpleCast(const DataType& t, PrimExpr value, Span span = Span()) {
  if (value.dtype() == t)
    return value;
  return PrimCast(t, value, span);
}

// LargeUIntImm
PrimExpr LargeUIntImm(DataType t, int64_t low, int64_t high, Span span) {
  return PrimCall(t,
                  builtin::large_uint_imm(),
                  {make_const(DataType::UInt(32), low), make_const(DataType::UInt(32), high)},
                  span);
}

// Q-multiplication
PrimExpr q_multiply_shift(PrimExpr x, PrimExpr y, PrimExpr q, PrimExpr s, Span span) {
  return PrimCall(
      DataType::Int(32, x.dtype().lanes()), builtin::q_multiply_shift(), {x, y, q, s}, span);
}

// The public function with a quick checking path.
void BinaryOpMatchTypes(PrimExpr& lhs, PrimExpr& rhs) {  // NOLINT(*)
  if (lhs.dtype() == rhs.dtype())
    return;
  DataType ltype = lhs.dtype();
  DataType rtype = rhs.dtype();

  MXCHECK(ltype.lanes() == rtype.lanes()) << "Cannot match type " << ltype << " vs " << rtype;
  if (lhs.dtype() == rhs.dtype())
    return;
  // Only do very simple type coversion
  // int->float, DataType::Int(32)->int(64)
  // require the types to be relatively consistent
  // This will the reduce amount code generated by operators
  // and also help user to find potential type conversion problems.
  if (!lhs.dtype().is_float() && (rhs.dtype().is_float())) {
    // int->float
    lhs = cast(rhs.dtype(), lhs);
  } else if ((lhs.dtype().is_float()) && !rhs.dtype().is_float()) {
    // int->float
    rhs = cast(lhs.dtype(), rhs);
  } else if ((lhs.dtype().is_int() && rhs.dtype().is_int()) ||
             (lhs.dtype().is_uint() && rhs.dtype().is_uint())) {
    // promote int to higher bits
    if (lhs.dtype().bits() < rhs.dtype().bits()) {
      lhs = cast(rhs.dtype(), lhs);
    } else {
      rhs = cast(lhs.dtype(), rhs);
    }
  } else if ((lhs.dtype().is_int() && rhs.dtype().is_uint()) ||
             (lhs.dtype().is_uint() && rhs.dtype().is_int())) {
    int bits = std::max(lhs.dtype().bits(), rhs.dtype().bits());
    lhs = SimpleCast(DataType::Int(bits, lhs.dtype().lanes()), lhs);
    rhs = SimpleCast(DataType::Int(bits, rhs.dtype().lanes()), rhs);
  } else {
    MXLOG(FATAL) << "Cannot match type " << ltype << " vs " << rtype;
  }
}

// maximum and min limits
PrimExpr max_value(const DataType& dtype, Span span) {
  MXCHECK_EQ(dtype.lanes(), 1);
  if (dtype.is_int()) {
    if (dtype.bits() == 64) {
      return IntImm(dtype, std::numeric_limits<int64_t>::max(), span);
    } else if (dtype.bits() < 64) {
      int64_t val = 1;
      val = (val << (dtype.bits() - 1)) - 1;
      return IntImm(dtype, val, span);
    }
  } else if (dtype.is_uint()) {
    if (dtype.bits() == 64) {
      return make_const(dtype, std::numeric_limits<uint64_t>::max(), span);
    } else if (dtype.bits() < 64) {
      uint64_t val = 1;
      val = (val << static_cast<uint64_t>(dtype.bits())) - 1;
      return IntImm(dtype, static_cast<int64_t>(val), span);
    }
  } else if (dtype.is_float()) {
    if (dtype.bits() == 64) {
      return FloatImm(dtype, std::numeric_limits<double>::max(), span);
    } else if (dtype.bits() == 32) {
      return FloatImm(dtype, std::numeric_limits<float>::max(), span);
    } else if (dtype.bits() == 16) {
      return FloatImm(dtype, 65504.0, span);
    }
  }
  MXLOG(FATAL) << "Cannot decide max_value for type" << dtype;
  return PrimExpr();
}

PrimExpr min_value(const DataType& dtype, Span span) {
  MXCHECK_EQ(dtype.lanes(), 1);
  if (dtype.is_int()) {
    if (dtype.bits() == 64) {
      return IntImm(dtype, std::numeric_limits<int64_t>::lowest(), span);
    } else if (dtype.bits() < 64) {
      int64_t val = 1;
      val = -(val << (dtype.bits() - 1));
      return IntImm(dtype, val, span);
    }
  } else if (dtype.is_uint()) {
    return IntImm(dtype, 0, span);
  } else if (dtype.is_float()) {
    if (dtype.bits() == 64) {
      return FloatImm(dtype, std::numeric_limits<double>::lowest(), span);
    } else if (dtype.bits() == 32) {
      return FloatImm(dtype, std::numeric_limits<float>::lowest(), span);
    } else if (dtype.bits() == 16) {
      return FloatImm(dtype, -65504.0, span);
    }
  }
  MXLOG(FATAL) << "Cannot decide min_value for type" << dtype;
  return PrimExpr();
}

// infinity
PrimExpr infinity(const DataType& dtype, Span span) {
  MXCHECK_EQ(dtype.lanes(), 1);
  if (dtype.is_float()) {
    if (dtype.bits() == 64) {
      return FloatImm(dtype, std::numeric_limits<double>::infinity(), span);
    } else if (dtype.bits() == 32 || dtype.bits() == 16) {
      return FloatImm(dtype, std::numeric_limits<float>::infinity(), span);
    }
  }
  MXLOG(FATAL) << "Cannot decide infinity for type " << dtype;
  return PrimExpr();
}

template <typename ValueType>
inline bool ConstPowerHelper(ValueType val, int* shift) {
  if (val <= 0)
    return false;
  shift[0] = 0;
  while (val != 0) {
    if (val & 1) {
      return (val == 1);
    }
    ++shift[0];
    val = val >> 1;
  }
  return true;
}

bool is_const_power_of_two_integer(const PrimExpr& x, int* shift) {
  if (const auto* op = x.as<IntImmNode>()) {
    return ConstPowerHelper(op->value, shift);
  } else {
    return false;
  }
}

PrimExpr cast(const DataType& t, PrimExpr value, Span span) {
  if (value.dtype() == t)
    return value;
  // const fold IntImm as they are used in index computations
  if (t.lanes() == 1) {
    if (const IntImmNode* op = value.as<IntImmNode>()) {
      return make_const(t, op->value, span);
    } else if (const FloatImmNode* op = value.as<FloatImmNode>()) {
      return make_const(t, op->value, span);
    }
    return PrimCast(t, value, span);
  } else {
    MXCHECK(value.dtype().lanes() == t.lanes());
    return PrimCast(t, value, span);
  }
}

// reinterpret
PrimExpr reinterpret(const DataType& t, PrimExpr value, Span span) {
  if (value.dtype() == t)
    return value;
  return PrimCall(t, builtin::reinterpret(), {value}, span);
}

// add
PrimExpr add(PrimExpr a, PrimExpr b, Span span) {
  BinaryOpMatchTypes(a, b);
  PrimExpr ret = arith::TryConstFold<PrimAdd>(a, b);
  if (ret.defined())
    return ret;
  return PrimAdd(a, b, span);
}

// negation
PrimExpr neg(PrimExpr a, Span span) {
  const IntImmNode* pa = a.as<IntImmNode>();
  const FloatImmNode* fa = a.as<FloatImmNode>();
  if (pa)
    return IntImm(a.dtype(), -pa->value, span);
  if (fa)
    return FloatImm(a.dtype(), -fa->value, span);
  return sub(make_zero(a.dtype()), a);
}

PrimExpr sub(PrimExpr a, PrimExpr b, Span span) {
  BinaryOpMatchTypes(a, b);
  PrimExpr ret = arith::TryConstFold<PrimSub>(a, b);
  if (ret.defined())
    return ret;
  return PrimSub(a, b, span);
}

PrimExpr mul(PrimExpr a, PrimExpr b, Span span) {
  BinaryOpMatchTypes(a, b);
  PrimExpr ret = arith::TryConstFold<PrimMul>(a, b);
  if (ret.defined())
    return ret;
  return PrimMul(a, b, span);
}

PrimExpr div(PrimExpr a, PrimExpr b, Span span) {
  BinaryOpMatchTypes(a, b);

  a = cast(DataType::Float(64), a);
  b = cast(DataType::Float(64), b);

  PrimExpr ret = arith::TryConstFold<PrimDiv>(a, b);
  if (ret.defined())
    return ret;

  static auto op = Op::Get("ir.div");
  a = cast(DataType::Float(64), a);
  b = cast(DataType::Float(64), b);

  return PrimCall(DataType::Float(64), op, {a, b}, span);
}

MATXSCRIPT_IR_REGISTER_PURE_BINARY_OP("ir.div")
    .set_attr<TVectorizable>("TVectorizable", true)
    .set_attr<TGlobalSymbol>("TGlobalSymbol", "ArithOps::div");

PrimExpr truncdiv(PrimExpr a, PrimExpr b, Span span) {
  return floordiv(a, b, span);
}

PrimExpr truncmod(PrimExpr a, PrimExpr b, Span span) {
  BinaryOpMatchTypes(a, b);
  PrimExpr ret = arith::TryConstFold<PrimMod>(a, b);
  if (ret.defined())
    return ret;
  return PrimMod(a, b, span);
}

// TODO(tqchen): switch to floordiv
PrimExpr indexdiv(PrimExpr a, PrimExpr b, Span span) {
  return floordiv(a, b, span);
}

PrimExpr indexmod(PrimExpr a, PrimExpr b, Span span) {
  return floormod(a, b, span);
}

PrimExpr floordiv(PrimExpr a, PrimExpr b, Span span) {
  bool is_both_int = false;
  if ((a.dtype().is_int() || a.dtype().is_uint()) && (b.dtype().is_int() || b.dtype().is_uint())) {
    is_both_int = true;
  }

  BinaryOpMatchTypes(a, b);
  PrimExpr ret = arith::TryConstFold<PrimFloorDiv>(a, b);
  if (ret.defined())
    return ret;

  static auto op = Op::Get("ir.floordiv");

  if (is_both_int) {
    runtime::Array<PrimExpr> args{a, b};
    return PrimCall(DataType::Int(64), op, std::move(args), span);
  }

  a = cast(DataType::Float(64), a);
  b = cast(DataType::Float(64), b);

  PrimExpr result = PrimCall(a.dtype(), op, {a, b}, span);
  return result;
}

MATXSCRIPT_IR_REGISTER_PURE_BINARY_OP("ir.floordiv")
    .set_attr<TVectorizable>("TVectorizable", true)
    .set_attr<TGlobalSymbol>("TGlobalSymbol", "ArithOps::floordiv");

PrimExpr floormod(PrimExpr a, PrimExpr b, Span span) {
  bool a_is_int = a.dtype().is_int() || a.dtype().is_uint();
  bool b_is_int = b.dtype().is_int() || b.dtype().is_uint();
  bool is_both_int = a_is_int && b_is_int;

  BinaryOpMatchTypes(a, b);
  PrimExpr ret = arith::TryConstFold<PrimFloorMod>(a, b);
  if (ret.defined())
    return ret;

  static auto op = Op::Get("ir.floormod");

  if (is_both_int) {
    runtime::Array<PrimExpr> args{a, b};
    return PrimCall(DataType::Int(64), op, std::move(args), span);
  }

  a = cast(DataType::Float(64), a);
  b = cast(DataType::Float(64), b);

  PrimExpr result = PrimCall(a.dtype(), op, {a, b}, span);
  return result;
}

MATXSCRIPT_IR_REGISTER_PURE_BINARY_OP("ir.floormod")
    .set_attr<TVectorizable>("TVectorizable", true)
    .set_attr<TGlobalSymbol>("TGlobalSymbol", "ArithOps::floormod");

PrimExpr min(PrimExpr a, PrimExpr b, Span span) {
  // inf-aware simplificaiton
  using arith::is_neg_inf;
  using arith::is_pos_inf;
  if (is_pos_inf(a))
    return b;
  if (is_neg_inf(a))
    return a;
  if (is_pos_inf(b))
    return a;
  if (is_neg_inf(b))
    return b;
  BinaryOpMatchTypes(a, b);
  PrimExpr ret = arith::TryConstFold<PrimMin>(a, b);
  if (ret.defined())
    return ret;
  return PrimMin(a, b, span);
}

PrimExpr max(PrimExpr a, PrimExpr b, Span span) {
  // inf-aware simplificaiton
  using arith::is_neg_inf;
  using arith::is_pos_inf;
  if (is_pos_inf(a))
    return a;
  if (is_neg_inf(a))
    return b;
  if (is_pos_inf(b))
    return b;
  if (is_neg_inf(b))
    return a;
  BinaryOpMatchTypes(a, b);
  PrimExpr ret = arith::TryConstFold<PrimMax>(a, b);
  if (ret.defined())
    return ret;
  return PrimMax(a, b, span);
}

// if_then_else
PrimExpr if_then_else(PrimExpr cond, PrimExpr true_value, PrimExpr false_value, Span span) {
  MXCHECK(cond.dtype() == DataType::Bool(1))
      << "if_then_else only accept the condition to be boolean type.";
  BinaryOpMatchTypes(true_value, false_value);
  if (const IntImmNode* op = cond.as<IntImmNode>()) {
    if (op->value != 0) {
      return true_value;
    } else {
      return false_value;
    }
  }

  return PrimCall(
      true_value.dtype(), builtin::if_then_else(), {cond, true_value, false_value}, span);
}

// likely
PrimExpr likely(PrimExpr cond, Span span) {
  if (is_const_int(cond))
    return cond;
  return PrimCall(cond.dtype(), builtin::likely(), {cond}, span);
}

// operator>
PrimExpr greater_than(PrimExpr a, PrimExpr b, Span span) {
  BinaryOpMatchTypes(a, b);
  PrimExpr ret = arith::TryConstFold<PrimGT>(a, b);
  if (ret.defined())
    return ret;
  return PrimGT(a, b, span);
}

PrimExpr greater_or_equal(PrimExpr a, PrimExpr b, Span span) {
  BinaryOpMatchTypes(a, b);
  PrimExpr ret = arith::TryConstFold<PrimGE>(a, b);
  if (ret.defined())
    return ret;
  return PrimGE(a, b, span);
}

PrimExpr less_than(PrimExpr a, PrimExpr b, Span span) {
  BinaryOpMatchTypes(a, b);
  PrimExpr ret = arith::TryConstFold<PrimLT>(a, b);
  if (ret.defined())
    return ret;
  return PrimLT(a, b, span);
}

PrimExpr less_or_equal(PrimExpr a, PrimExpr b, Span span) {
  BinaryOpMatchTypes(a, b);
  PrimExpr ret = arith::TryConstFold<PrimLE>(a, b);
  if (ret.defined())
    return ret;
  return PrimLE(a, b, span);
}

PrimExpr equal(PrimExpr a, PrimExpr b, Span span) {
  BinaryOpMatchTypes(a, b);
  PrimExpr ret = arith::TryConstFold<PrimEQ>(a, b);
  if (ret.defined())
    return ret;
  return PrimEQ(a, b, span);
}

PrimExpr not_equal(PrimExpr a, PrimExpr b, Span span) {
  BinaryOpMatchTypes(a, b);
  PrimExpr ret = arith::TryConstFold<PrimNE>(a, b);
  if (ret.defined())
    return ret;
  return PrimNE(a, b, span);
}

PrimExpr logic_and(PrimExpr a, PrimExpr b, Span span) {
  MXCHECK(a.dtype().is_bool() || a.dtype().is_int());
  MXCHECK(b.dtype().is_bool() || b.dtype().is_int());
  PrimExpr ret = arith::TryConstFold<PrimAnd>(a, b);
  if (ret.defined())
    return ret;
  return PrimAnd(a, b, span);
}

PrimExpr logic_or(PrimExpr a, PrimExpr b, Span span) {
  MXCHECK(a.dtype().is_bool() || a.dtype().is_int());
  MXCHECK(b.dtype().is_bool() || b.dtype().is_int());
  PrimExpr ret = arith::TryConstFold<PrimOr>(a, b);
  if (ret.defined())
    return ret;
  return PrimOr(a, b, span);
}

PrimExpr logic_not(PrimExpr a, Span span) {
  MXCHECK(a.dtype().is_bool() || a.dtype().is_int());
  PrimExpr ret = arith::TryConstFold<PrimNot>(a);
  if (ret.defined())
    return ret;
  return PrimNot(a, span);
}

// shirt right
PrimExpr right_shift(PrimExpr a, PrimExpr b, Span span) {
  MXCHECK(a.dtype().is_int() || a.dtype().is_uint());
  MXCHECK(b.dtype().is_int() || b.dtype().is_uint());
  BinaryOpMatchTypes(a, b);
  MATXSCRIPT_INDEX_CONST_PROPAGATION({
    const DataType& rtype = a.dtype();
    if (pb)
      MXCHECK(pb->value >= 0 && pb->value < rtype.bits())
          << "Shift amount must be non-negative and less than " << rtype.bits() << " for type "
          << rtype;
    if (pa && pb)
      return IntImm(rtype, (pa->value >> pb->value), span);
    if (pb) {
      if (pb->value == 0)
        return a;
    }
  });

  return PrimCall(a.dtype(), builtin::shift_right(), {a, b}, span);
}

// shift left
PrimExpr left_shift(PrimExpr a, PrimExpr b, Span span) {
  MXCHECK(a.dtype().is_int() || a.dtype().is_uint());
  MXCHECK(b.dtype().is_int() || b.dtype().is_uint());
  BinaryOpMatchTypes(a, b);
  MATXSCRIPT_INDEX_CONST_PROPAGATION({
    const DataType& rtype = a.dtype();
    if (pb)
      MXCHECK(pb->value >= 0 && pb->value < rtype.bits())
          << "Shift amount must be non-negative and less than " << rtype.bits() << " for type "
          << rtype;
    if (pa && pb)
      return IntImm(rtype, (pa->value << pb->value), span);
    if (pb) {
      if (pb->value == 0)
        return a;
    }
  });
  return PrimCall(a.dtype(), builtin::shift_left(), {a, b}, span);
}

// bitwise and
PrimExpr bitwise_and(PrimExpr a, PrimExpr b, Span span) {
  MXCHECK(a.dtype().is_int() || a.dtype().is_uint());
  MXCHECK(b.dtype().is_int() || b.dtype().is_uint());
  BinaryOpMatchTypes(a, b);
  MATXSCRIPT_INDEX_CONST_PROPAGATION({
    const DataType& rtype = a.dtype();
    if (pa && pb)
      return IntImm(rtype, (pa->value & pb->value), span);
  });
  return PrimCall(a.dtype(), builtin::bitwise_and(), {a, b}, span);
}

// bitwise_or
PrimExpr bitwise_or(PrimExpr a, PrimExpr b, Span span) {
  MXCHECK(a.dtype().is_int() || a.dtype().is_uint());
  MXCHECK(b.dtype().is_int() || b.dtype().is_uint());
  BinaryOpMatchTypes(a, b);
  MATXSCRIPT_INDEX_CONST_PROPAGATION({
    const DataType& rtype = a.dtype();
    if (pa && pb)
      return IntImm(rtype, (pa->value | pb->value), span);
  });
  return PrimCall(a.dtype(), builtin::bitwise_or(), {a, b}, span);
}

// bitwise_xor
PrimExpr bitwise_xor(PrimExpr a, PrimExpr b, Span span) {
  MXCHECK(a.dtype().is_int() || a.dtype().is_uint());
  MXCHECK(b.dtype().is_int() || b.dtype().is_uint());
  BinaryOpMatchTypes(a, b);
  MATXSCRIPT_INDEX_CONST_PROPAGATION({
    const DataType& rtype = a.dtype();
    if (pa && pb)
      return IntImm(rtype, (pa->value ^ pb->value), span);
  });
  return PrimCall(a.dtype(), builtin::bitwise_xor(), {a, b}, span);
}

// bitwie_not
PrimExpr bitwise_not(PrimExpr a, Span span) {
  MXCHECK(a.dtype().is_int() || a.dtype().is_uint());
  return PrimCall(a.dtype(), builtin::bitwise_not(), {a}, span);
}

MATXSCRIPT_REGISTER_GLOBAL("ir.bitwise_not").set_body_typed([](PrimExpr a, Span span) {
  return bitwise_not(a, span);
});

// pow
PrimExpr pow(PrimExpr x, PrimExpr y, Span span) {
  BinaryOpMatchTypes(x, y);
  static auto op = Op::Get("ir.pow");
  return PrimCall(x.dtype(), op, {x, y}, span);
}

// abs
PrimExpr abs(PrimExpr x, Span span) {
  if (x.dtype().is_int()) {
    const IntImmNode* px = x.as<IntImmNode>();
    if (px) {
      return IntImm(x.dtype(), std::abs(px->value), span);
    }
    return PrimSelect(greater_or_equal(x, make_zero(x.dtype())), x, neg(x));
  } else if (x.dtype().is_float()) {
    const FloatImmNode* fx = x.as<FloatImmNode>();
    if (fx) {
      return FloatImm(x.dtype(), std::fabs(fx->value), span);
    }
    static auto op = Op::Get("ir.fabs");
    return PrimCall(x.dtype(), op, {x}, span);
  } else if (x.dtype().is_uint()) {
    return x;
  } else {
    MXLOG(FATAL) << "Data type " << x.dtype()
                 << " not supported for absolute op. Skipping absolute op...";
    return x;
  }
}

MATXSCRIPT_IR_REGISTER_PURE_UNARY_OP("ir.fabs")
    .set_attr<TVectorizable>("TVectorizable", true)
    .set_attr<TGlobalSymbol>("TGlobalSymbol", "fabs");

// isnan
PrimExpr isnan(PrimExpr x, Span span) {
  DataType t = DataType::Bool(x.dtype().lanes());
  if (x.dtype().is_int() || x.dtype().is_uint()) {
    return make_const(t, false, span);
  } else if (x.dtype().is_float()) {
    const FloatImmNode* fx = x.as<FloatImmNode>();
    if (fx) {
      return make_const(t, std::isnan(fx->value), span);
    }
    static auto op = Op::Get("ir.isnan");
    if (x.dtype().bits() == 16) {
      return PrimCall(t, op, {cast(DataType::Float(32, t.lanes()), std::move(x))}, span);
    } else {
      return PrimCall(t, op, {x}, span);
    }
  } else {
    MXLOG(FATAL) << "Data type " << x.dtype()
                 << " not supported for isnan op. Skipping isnan op...";
    return x;
  }
}

// isinf
PrimExpr isinf(PrimExpr x, Span span) {
  DataType t = DataType::Bool(x.dtype().lanes());
  if (x.dtype().is_int() || x.dtype().is_uint()) {
    return make_const(t, false, span);
  } else if (x.dtype().is_float()) {
    PrimExpr infX = infinity(x.dtype());
    return logic_and(equal(abs(x), infX), logic_not(isnan(x)), span);
  } else {
    MXLOG(FATAL) << "Data type " << x.dtype()
                 << " not supported for finiteness ops. Skipping it...";
    return x;
  }
}

// isfinite
PrimExpr isfinite(PrimExpr x, Span span) {
  return logic_and(logic_not(isinf(x)), logic_not(isnan(x)), span);
}

// fmod
PrimExpr fmod(PrimExpr x, PrimExpr y, Span span) {
  BinaryOpMatchTypes(x, y);
  MXCHECK(x.dtype().is_float()) << "fmod only applies to float";
  static auto op = Op::Get("ir.fmod");
  return PrimCall(x.dtype(), op, {x, y}, span);
}

MATXSCRIPT_IR_REGISTER_PURE_UNARY_OP("ir.fmod").set_attr<TGlobalSymbol>("TGlobalSymbol", "fmod");

// floor
PrimExpr floor(PrimExpr x, Span span) {
  if (x.dtype().is_int() || x.dtype().is_uint()) {
    return x;
  }
  const FloatImmNode* fx = x.as<FloatImmNode>();
  if (fx)
    return FloatImm(x.dtype(), std::floor(fx->value), span);
  static auto op = Op::Get("ir.floor");
  PrimExpr result = PrimCall(x.dtype(), op, {x}, span);

  result = cast(DataType::Int(64), result);
  return result;
}

MATXSCRIPT_IR_REGISTER_PURE_UNARY_OP("ir.floor")
    .set_attr<TVectorizable>("TVectorizable", true)
    .set_attr<TGlobalSymbol>("TGlobalSymbol", "floor");

// ceil
PrimExpr ceil(PrimExpr x, Span span) {
  if (x.dtype().is_int() || x.dtype().is_uint()) {
    return x;
  }
  const FloatImmNode* fx = x.as<FloatImmNode>();
  if (fx)
    return FloatImm(x.dtype(), std::ceil(fx->value), span);
  static auto op = Op::Get("ir.ceil");
  PrimExpr result = PrimCall(x.dtype(), op, {x}, span);

  result = cast(DataType::Int(64), result);
  return result;
}

MATXSCRIPT_IR_REGISTER_PURE_UNARY_OP("ir.ceil")
    .set_attr<TVectorizable>("TVectorizable", true)
    .set_attr<TGlobalSymbol>("TGlobalSymbol", "ceil");

// round
PrimExpr round(PrimExpr x, Span span) {
  if (x.dtype().is_int() || x.dtype().is_uint()) {
    return x;
  }
  const FloatImmNode* fx = x.as<FloatImmNode>();
  if (fx)
    return FloatImm(x.dtype(), std::nearbyint(fx->value), span);
  static auto op = Op::Get("ir.round");
  return PrimCall(x.dtype(), op, {x}, span);
}

MATXSCRIPT_IR_REGISTER_PURE_UNARY_OP("ir.round")
    .set_attr<TVectorizable>("TVectorizable", true)
    .set_attr<TGlobalSymbol>("TGlobalSymbol", "round");

// nearbyint
PrimExpr nearbyint(PrimExpr x, Span span) {
  if (x.dtype().is_int() || x.dtype().is_uint()) {
    return x;
  }
  const FloatImmNode* fx = x.as<FloatImmNode>();
  if (fx)
    return FloatImm(x.dtype(), std::nearbyint(fx->value), span);
  static auto op = Op::Get("ir.nearbyint");
  return PrimCall(x.dtype(), op, {x}, span);
}

MATXSCRIPT_IR_REGISTER_PURE_UNARY_OP("ir.nearbyint")
    .set_attr<TGlobalSymbol>("TGlobalSymbol", "nearbyint");

// trunc
PrimExpr trunc(PrimExpr x, Span span) {
  if (x.dtype().is_int() || x.dtype().is_uint()) {
    return x;
  }
  const FloatImmNode* fx = x.as<FloatImmNode>();
  if (fx) {
    return FloatImm(
        x.dtype(), (fx->value < 0 ? std::ceil(fx->value) : std::floor(fx->value)), span);
  }
  static auto op = Op::Get("ir.trunc");
  return PrimCall(x.dtype(), op, {x}, span);
}

MATXSCRIPT_IR_REGISTER_PURE_UNARY_OP("ir.trunc")
    .set_attr<TVectorizable>("TVectorizable", true)
    .set_attr<TGlobalSymbol>("TGlobalSymbol", "trunc");

// unary op registration.
MATXSCRIPT_IR_REGISTER_PURE_BINARY_OP("ir.pow")
    .set_attr<TVectorizable>("TVectorizable", true)
    .set_attr<TGlobalSymbol>("TGlobalSymbol", "Math<double(double, double)>::check_call<pow>");

MATXSCRIPT_IR_REGISTER_PURE_UNARY_OP("ir.exp")
    .set_attr<TVectorizable>("TVectorizable", true)
    .set_attr<TGlobalSymbol>("TGlobalSymbol", "exp");

MATXSCRIPT_IR_REGISTER_PURE_UNARY_OP("ir.exp2")
    .set_attr<TVectorizable>("TVectorizable", true)
    .set_attr<TGlobalSymbol>("TGlobalSymbol", "exp2");

MATXSCRIPT_IR_REGISTER_PURE_UNARY_OP("ir.exp10")
    .set_attr<TVectorizable>("TVectorizable", true)
    .set_attr<TGlobalSymbol>("TGlobalSymbol", "exp10");

MATXSCRIPT_IR_REGISTER_PURE_UNARY_OP("ir.erf").set_attr<TGlobalSymbol>("TGlobalSymbol", "erf");

MATXSCRIPT_IR_REGISTER_PURE_UNARY_OP("ir.tanh")
    .set_attr<TVectorizable>("TVectorizable", true)
    .set_attr<TGlobalSymbol>("TGlobalSymbol", "tanh");

MATXSCRIPT_IR_REGISTER_PURE_UNARY_OP("ir.sigmoid")
    .set_attr<TGlobalSymbol>("TGlobalSymbol", "sigmoid");

MATXSCRIPT_IR_REGISTER_PURE_UNARY_OP("ir.sqrt")
    .set_attr<TVectorizable>("TVectorizable", true)
    .set_attr<TGlobalSymbol>("TGlobalSymbol", "Math<double(double)>::check_call<sqrt>");

MATXSCRIPT_IR_REGISTER_PURE_UNARY_OP("ir.rsqrt").set_attr<TGlobalSymbol>("TGlobalSymbol", "rsqrt");

MATXSCRIPT_IR_REGISTER_PURE_UNARY_OP("ir.log")
    .set_attr<TVectorizable>("TVectorizable", true)
    .set_attr<TGlobalSymbol>("TGlobalSymbol", "Math<double(double)>::check_call<log>");

MATXSCRIPT_IR_REGISTER_PURE_UNARY_OP("ir.log2")
    .set_attr<TVectorizable>("TVectorizable", true)
    .set_attr<TGlobalSymbol>("TGlobalSymbol", "Math<double(double)>::check_call<log2>");

MATXSCRIPT_IR_REGISTER_PURE_UNARY_OP("ir.log1p").set_attr<TGlobalSymbol>("TGlobalSymbol", "log1p");

MATXSCRIPT_IR_REGISTER_PURE_UNARY_OP("ir.log10")
    .set_attr<TVectorizable>("TVectorizable", true)
    .set_attr<TGlobalSymbol>("TGlobalSymbol", "Math<double(double)>::check_call<log10>");

MATXSCRIPT_IR_REGISTER_PURE_UNARY_OP("ir.tan")
    .set_attr<TVectorizable>("TVectorizable", true)
    .set_attr<TGlobalSymbol>("TGlobalSymbol", "tan");

MATXSCRIPT_IR_REGISTER_PURE_UNARY_OP("ir.cos")
    .set_attr<TVectorizable>("TVectorizable", true)
    .set_attr<TGlobalSymbol>("TGlobalSymbol", "cos");

MATXSCRIPT_IR_REGISTER_PURE_UNARY_OP("ir.cosh")
    .set_attr<TVectorizable>("TVectorizable", true)
    .set_attr<TGlobalSymbol>("TGlobalSymbol", "cosh");

MATXSCRIPT_IR_REGISTER_PURE_UNARY_OP("ir.sin")
    .set_attr<TVectorizable>("TVectorizable", true)
    .set_attr<TGlobalSymbol>("TGlobalSymbol", "sin");

MATXSCRIPT_IR_REGISTER_PURE_UNARY_OP("ir.sinh")
    .set_attr<TVectorizable>("TVectorizable", true)
    .set_attr<TGlobalSymbol>("TGlobalSymbol", "sinh");

MATXSCRIPT_IR_REGISTER_PURE_UNARY_OP("ir.asin").set_attr<TGlobalSymbol>("TGlobalSymbol", "asin");

MATXSCRIPT_IR_REGISTER_PURE_UNARY_OP("ir.acos").set_attr<TGlobalSymbol>("TGlobalSymbol", "acos");

MATXSCRIPT_IR_REGISTER_PURE_UNARY_OP("ir.atan").set_attr<TGlobalSymbol>("TGlobalSymbol", "atan");

MATXSCRIPT_IR_REGISTER_PURE_UNARY_OP("ir.acosh").set_attr<TGlobalSymbol>("TGlobalSymbol", "acosh");

MATXSCRIPT_IR_REGISTER_PURE_UNARY_OP("ir.asinh").set_attr<TGlobalSymbol>("TGlobalSymbol", "asinh");

MATXSCRIPT_IR_REGISTER_PURE_UNARY_OP("ir.atanh").set_attr<TGlobalSymbol>("TGlobalSymbol", "atanh");

// binary intrinsics
MATXSCRIPT_IR_REGISTER_PURE_BINARY_OP("ir.atan2").set_attr<TGlobalSymbol>("TGlobalSymbol", "atan2");

MATXSCRIPT_IR_REGISTER_PURE_BINARY_OP("ir.nextafter")
    .set_attr<TGlobalSymbol>("TGlobalSymbol", "nextafter");

MATXSCRIPT_IR_REGISTER_PURE_BINARY_OP("ir.hypot").set_attr<TGlobalSymbol>("TGlobalSymbol", "hypot");

MATXSCRIPT_IR_REGISTER_PURE_BINARY_OP("ir.copysign")
    .set_attr<TGlobalSymbol>("TGlobalSymbol", "copysign");

MATXSCRIPT_IR_REGISTER_PURE_BINARY_OP("ir.ldexp").set_attr<TGlobalSymbol>("TGlobalSymbol", "ldexp");

// expose basic functions to node namespace
MATXSCRIPT_REGISTER_GLOBAL("ir._const").set_body([](PyArgs args) -> RTValue {
  if (args[0].type_code() == TypeIndex::kRuntimeInteger) {
    return make_const(args[1].As<DataType>(), args[0].As<int64_t>());
  } else if (args[0].type_code() == TypeIndex::kRuntimeFloat) {
    return make_const(args[1].As<DataType>(), args[0].As<double>());
  } else {
    MXLOG(FATAL) << "only accept int or float";
  }
  return None;
});

// expose basic functions to node namespace
MATXSCRIPT_REGISTER_GLOBAL("runtime._const").set_body([](PyArgs args) -> RTValue {
  if (args[0].type_code() == TypeIndex::kRuntimeInteger) {
    return make_const(args[1].As<DataType>(), args[0].As<int64_t>());
  } else if (args[0].type_code() == TypeIndex::kRuntimeFloat) {
    return make_const(args[1].As<DataType>(), args[0].As<double>());
  } else {
    MXLOG(FATAL) << "only accept int or float";
  }
  return None;
});

MATXSCRIPT_REGISTER_GLOBAL("ir.LargeUIntImm").set_body_typed(LargeUIntImm);

MATXSCRIPT_REGISTER_GLOBAL("ir.min_value").set_body_typed(min_value);

MATXSCRIPT_REGISTER_GLOBAL("ir.max_value").set_body_typed(max_value);

MATXSCRIPT_REGISTER_GLOBAL("ir.abs").set_body_typed(abs);

MATXSCRIPT_REGISTER_GLOBAL("ir.isnan").set_body_typed(isnan);

MATXSCRIPT_REGISTER_GLOBAL("ir.isfinite").set_body_typed(isfinite);

MATXSCRIPT_REGISTER_GLOBAL("ir.isinf").set_body_typed(isinf);

MATXSCRIPT_REGISTER_GLOBAL("ir.floor").set_body_typed(floor);

MATXSCRIPT_REGISTER_GLOBAL("ir.ceil").set_body_typed(ceil);

MATXSCRIPT_REGISTER_GLOBAL("ir.round").set_body_typed(round);

MATXSCRIPT_REGISTER_GLOBAL("ir.nearbyint").set_body_typed(nearbyint);

MATXSCRIPT_REGISTER_GLOBAL("ir.trunc").set_body_typed(trunc);

MATXSCRIPT_REGISTER_GLOBAL("ir._cast").set_body_typed(cast);

// operator overloading, smarter than make
#define REGISTER_MAKE_UNARY_OP(Node, Func)                                           \
  MATXSCRIPT_REGISTER_GLOBAL("ir." #Node).set_body_typed([](PrimExpr a, Span span) { \
    return (Func(a, span));                                                          \
  })

#define REGISTER_MAKE_BINARY_OP(Node, Func)                                                      \
  MATXSCRIPT_REGISTER_GLOBAL("ir." #Node).set_body_typed([](PrimExpr a, PrimExpr b, Span span) { \
    return (Func(a, b, span));                                                                   \
  })

#define REGISTER_MAKE_BIT_OP(Node, Func)                                        \
  MATXSCRIPT_REGISTER_GLOBAL("ir." #Node).set_body([](PyArgs args) -> RTValue { \
    bool lhs_is_int = args[0].type_code() == TypeIndex::kRuntimeInteger;        \
    bool rhs_is_int = args[1].type_code() == TypeIndex::kRuntimeInteger;        \
    if (lhs_is_int) {                                                           \
      return (Func(args[0].As<int>(), args[1].As<PrimExpr>()));                 \
    } else if (rhs_is_int) {                                                    \
      return (Func(args[0].As<PrimExpr>(), args[1].As<int>()));                 \
    } else {                                                                    \
      return (Func(args[0].As<PrimExpr>(), args[1].As<PrimExpr>()));            \
    }                                                                           \
  })

// MATXSCRIPT_REGISTER_GLOBAL("ir._OpDiv").set_body_typed([](PrimExpr a, PrimExpr b, Span span =
// Span()) {
//   return (div(a, b, span));
// });

REGISTER_MAKE_BINARY_OP(_OpAdd, add);
REGISTER_MAKE_BINARY_OP(_OpSub, sub);
REGISTER_MAKE_BINARY_OP(_OpMul, mul);
REGISTER_MAKE_BINARY_OP(_OpDiv, div);
REGISTER_MAKE_BINARY_OP(_OpMod, truncmod);
REGISTER_MAKE_BINARY_OP(_OpIndexDiv, indexdiv);
REGISTER_MAKE_BINARY_OP(_OpIndexMod, indexmod);
REGISTER_MAKE_BINARY_OP(_OpFloorDiv, floordiv);
REGISTER_MAKE_BINARY_OP(_OpFloorMod, floormod);
REGISTER_MAKE_BINARY_OP(_OpTruncDiv, truncdiv);
REGISTER_MAKE_BINARY_OP(_OpTruncMod, truncmod);
REGISTER_MAKE_BINARY_OP(_OpMin, min);
REGISTER_MAKE_BINARY_OP(_OpMax, max);
REGISTER_MAKE_BINARY_OP(_OpEQ, equal);
REGISTER_MAKE_BINARY_OP(_OpNE, not_equal);
REGISTER_MAKE_BINARY_OP(_OpLT, less_than);      // NOLINT(*)
REGISTER_MAKE_BINARY_OP(_OpLE, less_or_equal);  // NOLINT(*)
REGISTER_MAKE_BINARY_OP(_OpGT, greater_than);   // NOLINT(*)
REGISTER_MAKE_BINARY_OP(_OpGE, greater_or_equal);
REGISTER_MAKE_BINARY_OP(_OpAnd, logic_and);
REGISTER_MAKE_BINARY_OP(_OpOr, logic_or);
REGISTER_MAKE_UNARY_OP(_OpNot, logic_not);
REGISTER_MAKE_BIT_OP(bitwise_and, bitwise_and);
REGISTER_MAKE_BIT_OP(bitwise_or, bitwise_or);
REGISTER_MAKE_BIT_OP(bitwise_xor, bitwise_xor);
REGISTER_MAKE_BIT_OP(left_shift, left_shift);  // NOLINT(*)
REGISTER_MAKE_BIT_OP(right_shift, right_shift);

MATXSCRIPT_REGISTER_GLOBAL("ir._OpIfThenElse")
    .set_body_typed(
        [](PrimExpr cond, PrimExpr true_value, PrimExpr false_value, Span span = Span()) {
          return if_then_else(cond, true_value, false_value, span);
        });

}  // namespace ir
}  // namespace matxscript