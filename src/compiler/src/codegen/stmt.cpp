/**
 * These codes are licensed under LICNSE_NAME License
 * See the LICENSE for details
 *
 * Copyright (c) 2022 Hiramoto Ittou
 */

#include <twinkle/codegen/stmt.hpp>
#include <twinkle/codegen/expr.hpp>
#include <twinkle/codegen/exception.hpp>

namespace twinkle::codegen
{

[[nodiscard]] SymbolTable mergeSymbolTables(const SymbolTable& a,
                                            const SymbolTable& b)
{
  SymbolTable merged_table{a};

  for (const auto& r : b) {
    // May be shadowed and must be overwriteable
    merged_table.insertOrAssign(r.first, r.second);
  }

  return merged_table;
}

//===----------------------------------------------------------------------===//
// Statement visitor
//===----------------------------------------------------------------------===//

struct StmtVisitor : public boost::static_visitor<void> {
  StmtVisitor(CGContext&         ctx,
              const SymbolTable& parent_scope,
              SymbolTable&       scope,
              const StmtContext& stmt_ctx) noexcept
    : ctx{ctx}
    , parent_scope{parent_scope}
    , scope{scope}
    , stmt_ctx{stmt_ctx}
  {
  }

  // Empty statement
  void operator()(boost::blank) const
  {
  }

  void operator()(const ast::CompoundStatement& node) const
  {
    createStatement(ctx, getAllSymbols(), stmt_ctx, node);
  }

  void operator()(const ast::Expr& node) const
  {
    static_cast<void>(createExpr(ctx, getAllSymbols(), stmt_ctx, node));
  }

  void operator()(const ast::Return& node) const
  {
    if (node.rhs) {
      auto const retval = createExpr(ctx, getAllSymbols(), stmt_ctx, *node.rhs);

      auto const return_type
        = ctx.return_type_table[ctx.builder.GetInsertBlock()->getParent()];

      assert(return_type);

      if (!equals(ctx, *return_type, retval.getType())) {
        throw CodegenError{
          ctx.formatError(ctx.positions.position_of(node),
                          "incompatible type for result type")};
      }

      ctx.builder.CreateStore(retval.getValue(), stmt_ctx.return_var);
    }

    ctx.builder.CreateBr(stmt_ctx.destruct_bb);
  }

  void operator()(const ast::VariableDef& node) const
  {
    if (!node.type && !node.initializer) {
      throw CodegenError{
        ctx.formatError(ctx.positions.position_of(node),
                        "type inference requires an initializer")};
    }

    const auto name = node.name.utf8();

    auto const func = ctx.builder.GetInsertBlock()->getParent();

    const auto is_mutable
      = node.qualifier && (*node.qualifier == VariableQual::mutable_);

    if (node.type) {
      const auto type
        = createType(ctx, *node.type, ctx.positions.position_of(node));

      scope.insertOrAssign(
        name,
        std::make_shared<AllocaVariable>(
          createAllocaVariable(ctx.positions.position_of(node),
                               func,
                               name,
                               type,
                               node.initializer,
                               is_mutable)));
    }
    else {
      scope.insertOrAssign(
        name,
        std::make_shared<AllocaVariable>(
          createAllocaVariableTyInference(ctx.positions.position_of(node),
                                          func,
                                          name,
                                          node.initializer,
                                          is_mutable)));
    }
  }

  void operator()(const ast::Assignment& node) const
  {
    return createAssignment(node);
  }

  void operator()(const ast::ClassMemberInit& node) const
  {
    return createAssignment(node, false);
  }

  void operator()(const ast::PrefixIncrementDecrement& node) const
  {
    const auto pos = ctx.positions.position_of(node);

    const auto operand
      = createAssignableValue(node.operand, ctx.positions.position_of(node));

    const auto derefed_operand = createDereference(ctx, pos, operand);

    const auto one = Value{llvm::ConstantInt::get(ctx.builder.getInt32Ty(), 1),
                           std::make_shared<BuiltinType>(BuiltinTypeKind::i32)};

    switch (node.kind()) {
    case ast::PrefixIncrementDecrement::Kind::unknown:
      throw CodegenError{ctx.formatError(
        pos,
        fmt::format("unknown operator '{}' detected", node.opstr()))};

    case ast::PrefixIncrementDecrement::Kind::increment:
      ctx.builder.CreateStore(createAdd(ctx, derefed_operand, one).getValue(),
                              operand.getValue());
      return;

    case ast::PrefixIncrementDecrement::Kind::decrement:
      ctx.builder.CreateStore(
        // If lhs and rhs are reversed, it will not work correctly
        createSub(ctx, derefed_operand, one).getValue(),
        operand.getValue());
      return;
    }
  }

  void operator()(const ast::If& node) const
  {
    auto const func = ctx.builder.GetInsertBlock()->getParent();

    auto const then_bb = llvm::BasicBlock::Create(ctx.context, "if_then", func);
    auto const else_bb = llvm::BasicBlock::Create(ctx.context, "if_else");

    auto const merge_bb = llvm::BasicBlock::Create(ctx.context, "if_merge");

    auto const cond_value
      = createExpr(ctx, getAllSymbols(), stmt_ctx, node.condition);

    if (!cond_value.getLLVMType()->isIntegerTy()
        && !cond_value.getLLVMType()->isPointerTy()) {
      throw CodegenError{
        ctx.formatError(ctx.positions.position_of(node),
                        "condition type is incompatible with bool")};
    }

    auto const cond = ctx.builder.CreateICmp(
      llvm::ICmpInst::ICMP_NE,
      cond_value.getValue(),
      llvm::Constant::getNullValue(cond_value.getLLVMType()));

    ctx.builder.CreateCondBr(cond, then_bb, else_bb);

    // Then statement codegen
    ctx.builder.SetInsertPoint(then_bb);

    createStatement(ctx, getAllSymbols(), stmt_ctx, node.then_statement);

    if (!ctx.builder.GetInsertBlock()->getTerminator())
      ctx.builder.CreateBr(merge_bb);

    // Else statement codegen
    func->getBasicBlockList().push_back(else_bb);
    ctx.builder.SetInsertPoint(else_bb);

    if (node.else_statement)
      createStatement(ctx, getAllSymbols(), stmt_ctx, *node.else_statement);

    if (!ctx.builder.GetInsertBlock()->getTerminator())
      ctx.builder.CreateBr(merge_bb);

    func->getBasicBlockList().push_back(merge_bb);

    ctx.builder.SetInsertPoint(merge_bb);
  }

  void operator()(const ast::Loop& node) const
  {
    auto const func = ctx.builder.GetInsertBlock()->getParent();

    auto const body_bb
      = llvm::BasicBlock::Create(ctx.context, "loop_body", func);

    auto const loop_end_bb = llvm::BasicBlock::Create(ctx.context, "loop_end");

    ctx.builder.CreateBr(body_bb);
    ctx.builder.SetInsertPoint(body_bb);

    createStatement(ctx,
                    getAllSymbols(),
                    {stmt_ctx.destruct_bb,
                     stmt_ctx.return_var,
                     stmt_ctx.end_bb,
                     loop_end_bb,
                     body_bb},
                    node.body);

    if (!ctx.builder.GetInsertBlock()->getTerminator())
      ctx.builder.CreateBr(body_bb);

    func->getBasicBlockList().push_back(loop_end_bb);
    ctx.builder.SetInsertPoint(loop_end_bb);
  }

  void operator()(const ast::While& node) const
  {
    auto const func = ctx.builder.GetInsertBlock()->getParent();

    auto const cond_bb
      = llvm::BasicBlock::Create(ctx.context, "while_cond", func);
    auto const body_bb = llvm::BasicBlock::Create(ctx.context, "while_body");

    auto const loop_end_bb = llvm::BasicBlock::Create(ctx.context, "while_end");

    ctx.builder.CreateBr(cond_bb);
    ctx.builder.SetInsertPoint(cond_bb);

    auto const cond = ctx.builder.CreateICmp(
      llvm::ICmpInst::ICMP_NE,
      createExpr(ctx, getAllSymbols(), stmt_ctx, node.cond_expr).getValue(),
      llvm::ConstantInt::get(
        BuiltinType{BuiltinTypeKind::bool_}.getLLVMType(ctx),
        0));

    ctx.builder.CreateCondBr(cond, body_bb, loop_end_bb);

    func->getBasicBlockList().push_back(body_bb);
    ctx.builder.SetInsertPoint(body_bb);

    createStatement(ctx,
                    getAllSymbols(),
                    {stmt_ctx.destruct_bb,
                     stmt_ctx.return_var,
                     stmt_ctx.end_bb,
                     loop_end_bb,
                     cond_bb},
                    node.body);

    if (!ctx.builder.GetInsertBlock()->getTerminator())
      ctx.builder.CreateBr(cond_bb);

    func->getBasicBlockList().push_back(loop_end_bb);
    ctx.builder.SetInsertPoint(loop_end_bb);
  }

  void operator()(const ast::For& node) const
  {
    if (node.init_stmt)
      boost::apply_visitor(*this, *node.init_stmt);

    auto const func = ctx.builder.GetInsertBlock()->getParent();

    auto const cond_bb
      = llvm::BasicBlock::Create(ctx.context, "for_cond", func);
    auto const loop_bb = llvm::BasicBlock::Create(ctx.context, "for_loop");
    auto const body_bb = llvm::BasicBlock::Create(ctx.context, "for_body");

    auto const loop_end_bb = llvm::BasicBlock::Create(ctx.context, "for_end");

    const StmtContext new_stmt_ctx{stmt_ctx.destruct_bb,
                                   stmt_ctx.return_var,
                                   stmt_ctx.end_bb,
                                   loop_end_bb,
                                   loop_bb};

    ctx.builder.CreateBr(cond_bb);
    ctx.builder.SetInsertPoint(cond_bb);

    if (node.cond_expr) {
      auto const cond = ctx.builder.CreateICmp(
        llvm::ICmpInst::ICMP_NE,
        createExpr(ctx, getAllSymbols(), new_stmt_ctx, *node.cond_expr)
          .getValue(),
        llvm::ConstantInt::get(
          BuiltinType{BuiltinTypeKind::bool_}.getLLVMType(ctx),
          0));

      ctx.builder.CreateCondBr(cond, body_bb, loop_end_bb);
    }
    else {
      // If condition is absent, unconditionally true
      ctx.builder.CreateCondBr(
        llvm::ConstantInt::get(ctx.builder.getInt1Ty(), true),
        body_bb,
        loop_end_bb);
    }

    func->getBasicBlockList().push_back(body_bb);
    ctx.builder.SetInsertPoint(body_bb);

    createStatement(ctx, getAllSymbols(), new_stmt_ctx, node.body);

    if (!ctx.builder.GetInsertBlock()->getTerminator())
      ctx.builder.CreateBr(loop_bb);

    func->getBasicBlockList().push_back(loop_bb);
    ctx.builder.SetInsertPoint(loop_bb);

    // Generate loop statement
    if (node.loop_stmt)
      createStatement(ctx, getAllSymbols(), new_stmt_ctx, *node.loop_stmt);

    ctx.builder.CreateBr(cond_bb);

    func->getBasicBlockList().push_back(loop_end_bb);
    ctx.builder.SetInsertPoint(loop_end_bb);
  }

  void operator()(ast::Break) const
  {
    if (stmt_ctx.break_bb) // If in a loop
      ctx.builder.CreateBr(stmt_ctx.break_bb);
  }

  void operator()(ast::Continue) const
  {
    if (stmt_ctx.continue_bb) // If in a loop
      ctx.builder.CreateBr(stmt_ctx.continue_bb);
  }

private:
  [[nodiscard]] SymbolTable getAllSymbols() const
  {
    return mergeSymbolTables(parent_scope, scope);
  }

  void createAssignment(const ast::Assignment& node,
                        const bool             const_check = true) const
  {
    const auto lhs = createAssignableValue(node.lhs,
                                           ctx.positions.position_of(node),
                                           const_check);

    const auto rhs = createExpr(ctx, getAllSymbols(), stmt_ctx, node.rhs);

    verifyVariableType(ctx.positions.position_of(node), rhs.getType());

    auto const lhs_value
      = Value{ctx.builder.CreateLoad(lhs.getLLVMType()->getPointerElementType(),
                                     lhs.getValue()),
              lhs.getType()->getPointeeType(ctx)};

    switch (node.kind()) {
    case ast::Assignment::Kind::unknown:
      throw CodegenError{ctx.formatError(
        ctx.positions.position_of(node),
        fmt::format("unknown operator '{}' detected", node.opstr()))};

    case ast::Assignment::Kind::direct:
      ctx.builder.CreateStore(rhs.getValue(), lhs.getValue());
      return;

    case ast::Assignment::Kind::add:
      ctx.builder.CreateStore(createAdd(ctx, lhs_value, rhs).getValue(),
                              lhs.getValue());
      return;

    case ast::Assignment::Kind::sub:
      ctx.builder.CreateStore(createSub(ctx, lhs_value, rhs).getValue(),
                              lhs.getValue());
      return;

    case ast::Assignment::Kind::mul:
      ctx.builder.CreateStore(createMul(ctx, lhs_value, rhs).getValue(),
                              lhs.getValue());
      return;

    case ast::Assignment::Kind::div:
      ctx.builder.CreateStore(createDiv(ctx, lhs_value, rhs).getValue(),
                              lhs.getValue());
      return;

    case ast::Assignment::Kind::mod:
      ctx.builder.CreateStore(createMod(ctx, lhs_value, rhs).getValue(),
                              lhs.getValue());
      return;
    }
  }

  [[nodiscard]] Value createAssignableValue(const ast::Expr&    node,
                                            const PositionRange pos,
                                            const bool const_check = true) const
  {
    const auto value = createExpr(ctx, getAllSymbols(), stmt_ctx, node);

    if (const_check && !value.isMutable()) {
      throw CodegenError{
        ctx.formatError(pos, "assignment of read-only variable")};
    }

    if (value.getType()->isRefTy(ctx)) {
      // Since reference types wrap pointer types
      return value;
    }

    return {llvm::getPointerOperand(value.getValue()),
            std::make_shared<PointerType>(value.getType()),
            value.isMutable()};
  }

  void verifyVariableType(const PositionRange&         pos,
                          const std::shared_ptr<Type>& type) const
  {
    if (type->isVoidTy(ctx)) {
      throw CodegenError{
        ctx.formatError(pos, "variable has incomplete type 'void'")};
    }
  }

  [[nodiscard]] AllocaVariable
  createAllocaVariable(const PositionRange&            pos,
                       llvm::Function*                 func,
                       const std::string&              name,
                       const std::shared_ptr<Type>&    type,
                       const std::optional<ast::Expr>& initializer,
                       const bool                      is_mutable) const
  {
    verifyVariableType(pos, type);

    auto const alloca = createEntryAlloca(func, name, type->getLLVMType(ctx));

    if (!initializer) {
      return {
        {alloca, type},
        is_mutable
      };
    }

    auto const init_value
      = createExpr(ctx, getAllSymbols(), stmt_ctx, *initializer);

    if (!equals(ctx, type, init_value.getType()))
      throw CodegenError{ctx.formatError(pos, "invalid initializer type")};

    ctx.builder.CreateStore(init_value.getValue(), alloca);

    return {
      {alloca, type},
      is_mutable
    };
  }

  [[nodiscard]] AllocaVariable
  createAllocaVariableTyInference(const PositionRange&            pos,
                                  llvm::Function*                 func,
                                  const std::string&              name,
                                  const std::optional<ast::Expr>& initializer,
                                  const bool is_mutable) const
  {
    auto const init_value
      = createExpr(ctx, getAllSymbols(), stmt_ctx, *initializer);

    verifyVariableType(pos, init_value.getType());

    auto const alloca = createEntryAlloca(func, name, init_value.getLLVMType());

    ctx.builder.CreateStore(init_value.getValue(), alloca);

    return {
      {alloca, init_value.getType()},
      is_mutable
    };
  }

  CGContext& ctx;

  const SymbolTable& parent_scope;

  SymbolTable& scope;

  const StmtContext& stmt_ctx;
};

[[nodiscard]] llvm::Function* findDestructor(CGContext&         ctx,
                                             const std::string& class_name)
{
  ctx.ns_hierarchy.push({class_name, NamespaceKind::class_});

  const auto destructor
    = findFunction(ctx, ctx.mangler.mangleDestructorCall(ctx, class_name));

  ctx.ns_hierarchy.pop();

  return destructor;
}

// If destructor is not defined, nothing is done
void invokeDestructor(CGContext& ctx, const Value& this_)
{
  assert(this_.getType()->isClassTy(ctx));

  const auto destructor
    = findDestructor(ctx, this_.getType()->getClassName(ctx));

  if (destructor) {
    ctx.builder.CreateCall(destructor,
                           {llvm::getPointerOperand(this_.getValue())});
  }
}

// If destructor is not defined, nothing is done
void invokeDestructor(CGContext& ctx, const std::shared_ptr<Variable>& this_)
{
  assert(this_->getType()->isClassTy(ctx));

  const auto destructor
    = findDestructor(ctx, this_->getType()->getClassName(ctx));

  if (destructor)
    ctx.builder.CreateCall(destructor, {this_->getAllocaInst()});
}

static void createDestructBB(CGContext&         ctx,
                             const StmtContext& stmt_ctx,
                             const SymbolTable& symbols,
                             const bool         return_)
{
  ctx.builder.GetInsertBlock()->getParent()->getBasicBlockList().push_back(
    stmt_ctx.destruct_bb);
  ctx.builder.SetInsertPoint(stmt_ctx.destruct_bb);

  for (const auto& symbol : symbols) {
    if (symbol.second->getType()->isClassTy(ctx))
      invokeDestructor(ctx, symbol.second);
  }

  if (return_)
    ctx.builder.CreateBr(stmt_ctx.end_bb);
}

void createStatement(CGContext&         ctx,
                     const SymbolTable& scope_arg,
                     const StmtContext& stmt_ctx_arg,
                     const ast::Stmt&   statement)
{
  SymbolTable new_scope;

  auto new_stmt_ctx        = stmt_ctx_arg;
  new_stmt_ctx.destruct_bb = llvm::BasicBlock::Create(ctx.context, "destruct");

  if (statement.type() == typeid(ast::CompoundStatement)) {
    auto& statements = boost::get<ast::CompoundStatement>(statement);

    for (const auto& r : statements) {
      boost::apply_visitor(StmtVisitor{ctx, scope_arg, new_scope, new_stmt_ctx},
                           r);

      if (ctx.builder.GetInsertBlock()->getTerminator()) {
        // Terminators cannot be placed in the middle of a basic block
        // Therefore, break
        break;
      }
    }
  }
  else {
    boost::apply_visitor(StmtVisitor{ctx, scope_arg, new_scope, new_stmt_ctx},
                         statement);
  }

  // The presence of a terminator means that there was a return statement
  if (ctx.builder.GetInsertBlock()->getTerminator())
    createDestructBB(ctx, new_stmt_ctx, new_scope, true);
  else {
    ctx.builder.CreateBr(new_stmt_ctx.destruct_bb);
    createDestructBB(ctx, new_stmt_ctx, new_scope, false);
  }
}

} // namespace twinkle::codegen
