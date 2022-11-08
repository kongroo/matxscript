# Copyright 2022 ByteDance Ltd. and/or its affiliates.
#
# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.

import unittest
import matx
from typing import Any


class TestDynamicVarType(unittest.TestCase):

    def test_dynamic_generic_var_type(self):
        def dynamic_generic_var_type() -> Any:
            s = "hello"
            s = s.encode()
            return s

        py_ret = dynamic_generic_var_type()
        tx_ret = matx.script(dynamic_generic_var_type)()
        self.assertEqual(py_ret, tx_ret)

    def test_dynamic_loop_var_type(self):
        def dynamic_loop_var_type() -> Any:
            s = "hello"
            li = []
            for si in s:
                si = si.encode()
                li.append(si)
            return b' '.join(li)

        py_ret = dynamic_loop_var_type()
        tx_ret = matx.script(dynamic_loop_var_type)()
        self.assertEqual(py_ret, tx_ret)

    def test_dynamic_arg_type(self):
        def dynamic_arg_type(s: str) -> Any:
            s = s.encode()
            return s

        input_s = "hello"
        py_ret = dynamic_arg_type(input_s)
        tx_ret = matx.script(dynamic_arg_type)(input_s)
        self.assertEqual(py_ret, tx_ret)


if __name__ == "__main__":
    import logging

    logging.basicConfig(level=logging.INFO)
    unittest.main()