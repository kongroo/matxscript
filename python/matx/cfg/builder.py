# Copyright 2022 ByteDance Ltd. and/or its affiliates.
#
# Acknowledgement:
# 1) Torczon, L. and Cooper, M. ed., (2012). Ch9 - Data-Flow Analysis.
#    In: Engineering a compiler, 2nd ed. Texas: Elsevier, Inc, pp.495-519.
# 2) Torczon, L. and Cooper, M. ed., (2012). Ch8 - Introduction to optimization.
#    In: Engineering a compiler, 2nd ed. Texas: Elsevier, Inc, pp.445-457.
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
from typed_ast import ast3 as ast
from typing import List, Dict, Optional, Union
from .model import Block, BasicBlock, FunctionLabel
from .model import ASTNodeContext


class BasicBlocksBuilder(ast.NodeVisitor):
    """BasicBlocksBuilder for control flow graph

    BasicBlocksBuilder is an ast.NodeVisitor that can walk through
    a program's AST and iteratively build the corresponding Blocks.
    """

    def __init__(self,
                 ast_node_ctx: ASTNodeContext,
                 ast_node: Union[ast.stmt, List[ast.stmt]]) -> None:
        self.ast_node_ctx: ASTNodeContext = ast_node_ctx
        self.ast_node = ast_node
        self._cache = []

    def flush(self, **kwargs):
        """create a BasicBlock from _cache"""
        if len(self._cache) > 0:
            blk = BasicBlock.from_list(self._cache,
                                       self.ast_node_ctx,
                                       **kwargs)
            blk.name = "L" + str(blk.start_line)
            self._cache = []
            return blk
        return None

    def _append_cache(self, node: ast.stmt):
        if len(self._cache) == 0 or self._cache[-1] != node:
            self._cache.append(node)

    def generic_visit(self, node: ast.AST):
        if isinstance(node, (ast.stmt,)):
            self._append_cache(node)
        return super().generic_visit(node)

    def get_basic_block(self):
        if isinstance(self.ast_node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            yield from self.visit(self.ast_node)
        else:
            for node in self.ast_node:
                basic_block_list = self.visit(node)
                if basic_block_list is not None:
                    for block in basic_block_list:
                        yield block
            # yield the final block
            if len(self._cache) > 0:
                basic_block = self.flush()
                yield basic_block

    def visit_If(self, ast_node: ast.If):
        return self.visit_conditional_stmt(ast_node)

    def visit_While(self, ast_node: ast.While):
        return self.visit_conditional_stmt(ast_node)

    def visit_For(self, ast_node: ast.For):
        return self.visit_conditional_stmt(ast_node)

    def visit_Try(self, ast_node: ast.Try):
        return self.visit_conditional_stmt(ast_node)

    def visit_conditional_stmt(self, ast_node: Union[ast.stmt, ast.ExceptHandler]):
        self._append_cache(ast_node)
        basic_block = self.flush()
        basic_block.block_end_type = ast_node.__class__.__name__
        basic_block.name = "L" + str(basic_block.start_line)
        return [basic_block]

    def visit_FunctionDef(self, ast_node: Union[ast.FunctionDef, ast.AsyncFunctionDef]):
        block_list_generated = []
        block = self.flush()
        if block is not None:
            block_list_generated.append(block)
        # yield the function label
        function_block = FunctionLabel.from_ast(ast_node, self.ast_node_ctx)
        block_list_generated.append(function_block)
        return block_list_generated

    def visit_AsyncFunctionDef(self, ast_node: ast.AsyncFunctionDef):
        return self.visit_FunctionDef(ast_node)

    def visit_Return(self, ast_node: ast.Return):
        self._append_cache(ast_node)
        basic_block = self.flush(block_end_type=ast_node.__class__.__name__)
        return [basic_block]


class ForIter(ast.expr):
    _fields = ('target', 'iter')

    def __init__(self, target, iter_expr):
        super(ForIter, self).__init__()
        self.target = target
        self.iter = iter_expr

    def __repr__(self):
        return "ForIter: for {} in {}".format(self.target, repr(self.iter))


class TryLabel(ast.stmt):
    _fields = ()

    def __init__(self):
        super(TryLabel, self).__init__()

    def __repr__(self):
        return "TryLabel"


class ExceptHandlerLabel(ast.expr):
    _fields = ('type', 'name',)

    def __init__(self, t, n):
        super(ExceptHandlerLabel, self).__init__()
        self.type = t
        self.name = n

    def __repr__(self):
        return "ExceptHandlerLabel: {} as {}".format(self.type, self.name)


class LoadStoreSeparator(ast.NodeVisitor):
    def __init__(self):
        self.load = set()
        self.store = set()

    def visit_Name(self, node: ast.Name):
        if isinstance(node.ctx, ast.Store):
            self.store.add(node.id)
        else:
            self.load.add(node.id)


class CFG(object):
    """Control flow graph (CFG).

    A control flow graph is composed of basic blocks and links between them
    representing control flow jumps. It has a unique entry block and several
    possible 'final' blocks (blocks with no exits representing the end of the
    CFG).
    """

    def __init__(
            self,
            ast_tree: ast.AST,
    ) -> None:
        assert isinstance(
            ast_tree, ast.FunctionDef
        ), "Only functions are supported currently"
        self.ast_node_ctx: ASTNodeContext = ASTNodeContext(ast_tree)
        # the name of the function being represented
        self.name: str = ast_tree.name
        self.ast_tree = ast_tree
        # the entry block
        self.entry_block: Optional[Block] = None
        # all blocks
        self.block_list: List[Block] = []
        # global var
        self.globals_var = set()
        self.block_set: Dict[ast.AST, Block] = {}
        # build entry block
        self.entry_block, _, _ = self.parse(ast_tree)

    def __str__(self) -> str:
        return "CFG for {}".format(self.name)

    def add_basic_block(self, basic_block: BasicBlock):
        if basic_block.scope is not None:
            basic_block.scope.blocks.append(basic_block)
        self.block_list.append(basic_block)

    def link_tail_to_cur_block(self, all_tail_list: List[BasicBlock], basic_block: BasicBlock):
        for tail in all_tail_list:
            self.connect_2_blocks(tail, basic_block)
        all_tail_list[:] = []

    def build(self, block, head, all_tail_list, func_tail_list):
        method_str = "build_" + (block.block_end_type or '')
        build_method = getattr(self, method_str, self.build_generic)
        tail_list, func_tail = build_method(block)
        head = head or block
        self.link_tail_to_cur_block(all_tail_list, block)
        all_tail_list.extend(tail_list)
        func_tail_list.extend(func_tail)
        return head

    def build_If(self, if_block: BasicBlock):
        all_tail_list, func_tail_list = [], []
        ast_if_node = if_block.statements[-1]
        assert isinstance(ast_if_node, ast.If)
        if_block.statements[-1] = ast_if_node.test
        head_returned, tail_list, func_tail = self.parse(ast_if_node.body)
        self.connect_2_blocks(if_block, head_returned)
        all_tail_list.extend(tail_list)
        func_tail_list.extend(func_tail)
        head_returned, tail_list, func_tail = self.parse(ast_if_node.orelse)
        if head_returned is not None:
            # has an else or elif
            self.connect_2_blocks(if_block, head_returned)
            all_tail_list.extend(tail_list)
            func_tail_list.extend(func_tail)
        else:
            # link this to the next statement if no else
            all_tail_list.append(if_block)
        return all_tail_list, func_tail_list

    def build_FunctionDef(self, func_block: FunctionLabel):
        ast_fn_node = func_block.ast_node
        assert isinstance(ast_fn_node, ast.FunctionDef)
        head_returned, tail_list, func_tail_list = self.parse(ast_fn_node.body)
        self.connect_2_blocks(func_block, head_returned)
        func_block.func_tail.extend(func_tail_list)
        return [], []

    def build_Return(self, return_block: BasicBlock):
        self.build_generic(return_block)
        return [], [return_block]

    def build_While(self, while_block: BasicBlock):
        while_block = self.separate_block(while_block, "While")
        ast_while_node = while_block.statements[-1]
        assert isinstance(ast_while_node, ast.While)
        while_block.statements[-1] = ast_while_node.test
        head_returned, tail_list, func_tail = self.parse(ast_while_node.body)
        self.connect_2_blocks(while_block, head_returned)
        self.link_tail_to_cur_block(tail_list, while_block)
        return [while_block], func_tail

    def build_For(self, for_block: BasicBlock):
        func_tails = []
        for_block = self.separate_block(for_block, "For")
        ast_for_node = for_block.statements[-1]
        assert isinstance(ast_for_node, ast.For)
        for_iter = ForIter(ast_for_node.target, ast_for_node.iter)
        for_iter.lineno = ast_for_node.lineno
        for_iter.col_offset = ast_for_node.col_offset
        for_block.statements[-1] = for_iter
        head_returned, tail_list, func_tail = self.parse(ast_for_node.body)
        func_tails.extend(func_tail)
        self.connect_2_blocks(for_block, head_returned)
        self.link_tail_to_cur_block(tail_list, for_block)
        head_returned, tail_list, func_tail = self.parse(ast_for_node.orelse)
        func_tails.extend(func_tail)
        self.connect_2_blocks(for_block, head_returned)
        return [for_block, head_returned], func_tail

    def build_Try(self, try_block: BasicBlock):
        all_tail_list, func_tail_list = [], []
        ast_try_node = try_block.statements[-1]
        assert isinstance(ast_try_node, ast.Try)
        try_label = TryLabel()
        try_label.lineno = ast_try_node.lineno
        try_label.col_offset = ast_try_node.col_offset
        try_block.statements[-1] = try_label
        try_body_head, try_body_tail_list, try_body_func_tail = self.parse(ast_try_node.body)
        func_tail_list.extend(try_body_func_tail)
        has_finally = bool(ast_try_node.finalbody)
        has_except = bool(ast_try_node.handlers)
        has_else = bool(ast_try_node.orelse)
        if has_else:
            assert has_except, "syntax error: try ... else has no except"
        assert has_except or has_finally, "syntax error: try with no except or finally"

        handlers_label_blocks = []
        handlers_body_heads, handlers_body_tail, handlers_body_func_tail = [], [], []
        if has_except:
            for handler in ast_try_node.handlers:
                e_label = ExceptHandlerLabel(handler.type, handler.name)
                e_label.lineno = handler.lineno
                e_label.col_offset = handler.col_offset
                label_block = BasicBlock.from_list([e_label], self.ast_node_ctx)
                self.ast_node_ctx.set_block(handler, label_block)
                handlers_label_blocks.append(label_block)
                h_head, h_tail, h_func_tail = self.parse(handler.body)
                # except_body -> except
                self.connect_2_blocks(label_block, h_head)
                if h_head is not None:
                    handlers_body_heads.append(h_head)
                    handlers_body_tail.extend(h_tail)
                    handlers_body_func_tail.extend(h_func_tail)
            func_tail_list.extend(handlers_body_func_tail)
        else_block, else_tail, else_func_tail = [], [], []
        if has_else:
            else_block, else_tail, else_func_tail = self.parse(ast_try_node.orelse)
            func_tail_list.extend(else_func_tail)

        self.connect_2_blocks(try_block, try_body_head)  # try -> try_body
        if has_finally:
            final_body_head, finally_tail, finally_func_tail = self.parse(ast_try_node.finalbody)
            func_tail_list.extend(finally_func_tail)
            if not has_except:  # try finally
                self.connect_2_blocks(try_block, final_body_head)
                self.link_tail_to_cur_block(try_body_tail_list, final_body_head)
            else:
                for h_block in handlers_label_blocks:
                    # try -> except
                    self.connect_2_blocks(try_block, h_block)
                    # try_body -> except
                    self.link_tail_to_cur_block(try_body_tail_list, h_block)
                    # except -> finally
                    self.connect_2_blocks(h_block, final_body_head)
                # except_body -> finally
                self.link_tail_to_cur_block(handlers_body_tail, final_body_head)
                if has_else:  # try except else finally
                    # try_body -> else
                    self.link_tail_to_cur_block(try_body_tail_list, else_block)
                    # else_body -> finally
                    self.link_tail_to_cur_block(else_tail, final_body_head)
                else:  # try except finally
                    # try_body -> finally
                    self.link_tail_to_cur_block(try_body_tail_list, final_body_head)
            # finally is always the final tail block
            return finally_tail, func_tail_list
        else:
            assert has_except, "internal error: control flow graph"
            for h_block in handlers_label_blocks:
                # try -> except
                self.connect_2_blocks(try_block, h_block)
                # try_body -> except
                self.link_tail_to_cur_block(try_body_tail_list, h_block)
            all_tail_list.extend(handlers_body_tail)
            if has_else:  # try except else
                # try_body -> else
                self.link_tail_to_cur_block(try_body_tail_list, else_block)
                all_tail_list.extend(else_tail)
            else:
                # try except
                all_tail_list.extend(try_body_tail_list)
            return all_tail_list, func_tail_list

    def build_Call(self, basic_block: BasicBlock):
        return [basic_block], []

    def build_generic(self, basic_block):
        return [basic_block], []

    def parse(self, ast_body: Union[List[ast.ExceptHandler], ast.stmt, List[ast.stmt]]):
        head = None
        all_tail_list, func_tail_list = [], []
        basic_block_parser = BasicBlocksBuilder(self.ast_node_ctx, ast_body)
        for basic_block in basic_block_parser.get_basic_block():
            self.add_basic_block(basic_block)
            head = self.build(basic_block, head, all_tail_list, func_tail_list)
        return head, all_tail_list, func_tail_list

    def separate_block(self, basic_block: BasicBlock, block_end_type=""):
        if basic_block.start_line == basic_block.end_line:
            # if blocks only contain 1 line of code, it's not needed to be separated
            return basic_block
        else:
            separated_block = BasicBlock(start_line=basic_block.end_line,
                                         end_line=basic_block.end_line)
            separated_block.statements.append(basic_block.statements[-1])
            basic_block.statements = basic_block.statements[:-1]
            basic_block.end_line -= 1
            self.connect_2_blocks(basic_block, separated_block)
            separated_block.block_end_type = block_end_type
            separated_block.name = "L{}".format(separated_block.start_line)
            basic_block.block_end_type = ""
            self.add_basic_block(separated_block)
            return separated_block

    @staticmethod
    def connect_2_blocks(block1: Block, block2: Block):
        if block1 is not None and block2 is not None:
            block1.next_block_list.append(block2)
            block2.prev_block_list.append(block1)

    def fill_dominance_frontier(self):
        self.entry_block.fill_dominates(self.ast_node_ctx)
        self.entry_block.fill_immediate_dominators(self.ast_node_ctx)
        self.entry_block.fill_dominance_frontier(self.ast_node_ctx, self.block_list)

    def init_var_life_info(self):
        for block in self.block_list:
            for stmt in block.get_code_to_analyse():
                sep = LoadStoreSeparator()
                sep.visit(stmt)
                for load_var in sep.load:
                    load_var = load_var
                    if load_var not in block.var_kill:
                        block.ue_var.add(load_var)
                        self.globals_var.add(load_var)
                for store_var in sep.store:
                    block.var_kill.add(store_var)
                    self.block_set[store_var] = block

    def compute_live_out_var(self):
        changed_flag = True
        while changed_flag:
            changed_flag = False
            for blocks in self.block_list:
                if blocks.recompute_live_out_var():
                    changed_flag = True