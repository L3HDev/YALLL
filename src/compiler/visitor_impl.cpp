#include "visitor_impl.h"

#include <llvm/ADT/APInt.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/GenericValue.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Value.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetOptions.h>
#include <tree/ParseTree.h>
#include <tree/ParseTreeType.h>
#include <tree/ParseTreeWalker.h>

#include <any>
#include <iostream>
#include <memory>
#include <ostream>
#include <source_location>
#include <string>
#include <typeinfo>
#include <vector>

#include "../function/function.h"
#include "../operation/addoperation.h"
#include "../operation/andoperation.h"
#include "../operation/cmpoperation.h"
#include "../operation/funccalloperation.h"
#include "../operation/muloperation.h"
#include "../operation/operation.h"
#include "../operation/oroperation.h"
#include "../operation/terminaloperation.h"
#include "../scoping/scope.h"
#include "../value/value.h"
#include "YALLLParser.h"

namespace yallc {

// std::string get_assigned_name(YALLLParser::Terminal_opContext* ctx) {
//   antlr4::tree::ParseTree* parent = ctx->parent;
//
//   while (parent->parent) {
//     if (auto assigment =
//             dynamic_cast<YALLLParser::AssignmentContext*>(parent)) {
//       return assigment->name->getText();
//     }
//     if (auto var_def = dynamic_cast<YALLLParser::Var_defContext*>(parent)) {
//       return var_def->name->getText();
//     }
//     parent = parent->parent;
//   }
//   return "";
// }

inline void incompatible_types(typesafety::TypeInformation& lhs,
                               typesafety::TypeInformation& rhs, size_t line) {
  std::cout << "Incompatible types " << lhs.to_string() << " and "
            << rhs.to_string() << " in line " << line << std::endl;
}

inline yalll::Value to_value(
    std::any any,
    std::source_location location = std::source_location::current()) {
  try {
    return std::any_cast<yalll::Value>(any);
  } catch (const std::bad_any_cast& cast) {
    std::cout << location.file_name() << ":" << std::endl
              << location.function_name() << "@" << location.line()
              << ": Faild to cast " << any.type().name() << " to yalll::Value "
              << cast.what() << std::endl;
    throw cast;
  }
}

inline std::shared_ptr<yalll::Operation> to_operation(
    std::any any,
    std::source_location location = std::source_location::current()) {
  try {
    auto type = any.type().hash_code();
    if (type == typeid(std::shared_ptr<yalll::AddOperation>).hash_code())
      return std::any_cast<std::shared_ptr<yalll::AddOperation>>(any);
    if (type == typeid(std::shared_ptr<yalll::MulOperation>).hash_code())
      return std::any_cast<std::shared_ptr<yalll::MulOperation>>(any);
    if (type == typeid(std::shared_ptr<yalll::AndOperation>).hash_code())
      return std::any_cast<std::shared_ptr<yalll::AndOperation>>(any);
    if (type == typeid(std::shared_ptr<yalll::OrOperation>).hash_code())
      return std::any_cast<std::shared_ptr<yalll::OrOperation>>(any);
    if (type == typeid(std::shared_ptr<yalll::CmpOperation>).hash_code())
      return std::any_cast<std::shared_ptr<yalll::CmpOperation>>(any);
    if (type == typeid(std::shared_ptr<yalll::TerminalOperation>).hash_code())
      return std::any_cast<std::shared_ptr<yalll::TerminalOperation>>(any);
    if (type == typeid(std::shared_ptr<yalll::FuncCallOperation>).hash_code())
      return std::any_cast<std::shared_ptr<yalll::FuncCallOperation>>(any);

    return std::any_cast<std::shared_ptr<yalll::Operation>>(any);
  } catch (const std::bad_any_cast& cast) {
    std::cout << location.file_name() << ":" << std::endl
              << location.function_name() << "@" << location.line() << std::endl
              << ": Failed to cast " << any.type().name()
              << " to std::shared_ptr<yalll::Operation> " << cast.what()
              << std::endl;
    throw cast;
  }
}

YALLLVisitorImpl::YALLLVisitorImpl(std::string out_path) : out_path(out_path) {
  yalll::Import<llvm::LLVMContext> context;
  module = std::make_unique<llvm::Module>("YALLL", *context);
}

YALLLVisitorImpl::~YALLLVisitorImpl() {}

std::any YALLLVisitorImpl::visitProgram(YALLLParser::ProgramContext* ctx) {
  auto res = visitChildren(ctx);

  std::error_code ec;
  llvm::raw_fd_ostream llvm_out(out_path, ec);
  module->print(llvm_out, nullptr);

  return res;
}

std::any YALLLVisitorImpl::visitInterface(YALLLParser::InterfaceContext* ctx) {
  ++*logger;
  auto res = visitChildren(ctx);
  --*logger;
  return res;
}

std::any YALLLVisitorImpl::visitClass(YALLLParser::ClassContext* ctx) {
  ++*logger;
  auto res = visitChildren(ctx);
  --*logger;
  return res;
}

std::any YALLLVisitorImpl::visitEntry_point(
    YALLLParser::Entry_pointContext* ctx) {
  logger->send_log("Entering main function");
  ++*logger;

  cur_scope.push("main");
  yalll::Function func("main", typesafety::TypeInformation::I32_T(), true);

  (void)func.generate_function_sig(*module);
  cur_scope.add_function("main", std::move(func));
  cur_scope.set_active_function("main");

  visitChildren(ctx);

  // ensure error exit if no return given by program
  builder->CreateRet(llvm::ConstantInt::getSigned(builder->getInt32Ty(), 1));

  --*logger;
  return std::any();
}

// std::any YALLLVisitorImpl::visitStatement(YALLLParser::StatementContext* ctx)
// {
//
// }

std::any YALLLVisitorImpl::visitExpression(
    YALLLParser::ExpressionContext* ctx) {
  logger->send_log("Visiting expression");
  ++*logger;

  switch (ctx->getStart()->getType()) {
    case YALLLParser::RETURN_KW:
      logger->send_log<std::string>("Returning {}", ctx->ret_val->getText());

      auto operation = to_operation(visit(ctx->ret_val));
      if (cur_scope.has_active_function() &&
          operation->resolve_with_type_info(
              cur_scope.get_active_function()->get_return_type())) {
        cur_scope.get_active_function()->ret_val = operation->generate_value();
        logger->send_log("Return Info: {}",
                         cur_scope.get_active_function()->ret_val.to_string());
        cur_scope.get_active_function()->generate_function_return();

      } else if (operation->resolve_without_type_info()) {
        logger->send_internal_error("Returning without active function!");
        (void)builder->CreateRet(operation->generate_value().get_llvm_val());
      }

      --*logger;
      return std::any();
  }

  --*logger;
  return visitChildren(ctx);
}

std::any YALLLVisitorImpl::visitBlock(YALLLParser::BlockContext* ctx) {
  logger->send_log("Visiting block");
  ++*logger;

  cur_scope.push();
  for (auto* statement : ctx->statements) {
    logger->send_log("Statement: {}", statement->getText());
    visit(statement);
  }

  cur_scope.pop();
  --*logger;
  return std::any();
}

std::any YALLLVisitorImpl::visitAssignment(
    YALLLParser::AssignmentContext* ctx) {
  logger->send_log("Visiting assignment");
  ++*logger;
  auto* variable = cur_scope.find_field(ctx->name->getText());

  if (variable) {
    if (!variable->type_info.is_mutable() && variable->llvm_val) {
      logger->send_error("Trying to reassign immutable value {} in line {}",
                         ctx->name->getText(), ctx->name->getLine());
      --*logger;
      return std::any();
    }

    auto operation = to_operation(visit(ctx->val));
    if (operation->resolve_with_type_info(variable->type_info)) {
      variable->llvm_val = operation->generate_value().get_llvm_val();
    }
  } else {
    logger->send_error("Undeclared variable {} used in line {}",
                       ctx->name->getText(), ctx->name->getLine());
  }

  --*logger;
  return std::any();
}

std::any YALLLVisitorImpl::visitVar_dec(YALLLParser::Var_decContext* ctx) {
  logger->send_log("Visiting var dec");
  ++*logger;

  std::string name = ctx->name->getText();
  auto type_info = typesafety::TypeInformation::from_context_node(ctx->ty);

  cur_scope.add_field(
      name, yalll::Value(type_info, nullptr, ctx->name->getLine(), name));

  --*logger;
  return std::any();
}

std::any YALLLVisitorImpl::visitVar_def(YALLLParser::Var_defContext* ctx) {
  logger->send_log("Visiting var def");
  ++*logger;

  std::string name = ctx->name->getText();
  auto operation = to_operation(visit(ctx->val));

  auto type_info = typesafety::TypeInformation::from_context_node(ctx->ty);
  logger->send_log("Got {} with type: {}", name, type_info.to_string());

  if (operation->resolve_with_type_info(type_info)) {
    cur_scope.add_field(
        name,
        yalll::Value(type_info, operation->generate_value().get_llvm_val(),
                     ctx->getStart()->getLine(), name));
  }

  --*logger;
  return std::any();
}

std::any YALLLVisitorImpl::visitFunction_def(
    YALLLParser::Function_defContext* ctx) {
  std::string name = ctx->func_name->getText();

  logger->send_log("Visiting function {}", name);
  ++*logger;

  auto ret_type = typesafety::TypeInformation::from_context_node(ctx->ret_type);
  auto params = std::any_cast<std::vector<yalll::Value>>(visit(ctx->parm_list));

  cur_scope.push(name);
  yalll::Function func(name, ret_type, params);

  (void)func.generate_function_sig(*module);
  cur_scope.add_function(name, std::move(func));
  cur_scope.set_active_function(name);

  visit(ctx->func_block);

  --*logger;
  return std::any();
}
std::any YALLLVisitorImpl::visitParameter_list(
    YALLLParser::Parameter_listContext* ctx) {
  logger->send_log("Visiting paramlist");
  ++*logger;

  std::vector<yalll::Value> params;
  if (ctx->first_type) {
    auto type_info =
        typesafety::TypeInformation::from_context_node(ctx->first_type);
    std::string name = ctx->first_name->getText();
    params.push_back(
        yalll::Value(type_info, nullptr, ctx->getStart()->getLine(), name));
  }

  for (auto i = 0; i < ctx->nth_type.size(); ++i) {
    auto type_info =
        typesafety::TypeInformation::from_context_node(ctx->nth_type.at(i));
    std::string name = ctx->nth_name.at(i)->getText();
    params.push_back(
        yalll::Value(type_info, nullptr, ctx->getStart()->getLine(), name));
  }

  --*logger;
  return params;
}

std::any YALLLVisitorImpl::visitIf_else(YALLLParser::If_elseContext* ctx) {
  logger->send_log("Visiting if else");
  ++*logger;

  auto if_true = llvm::BasicBlock::Create(
      *context, "if_true", module->getFunction(cur_scope.get_scope_ctx_name()));
  auto if_false = llvm::BasicBlock::Create(
      *context, "if_false",
      module->getFunction(cur_scope.get_scope_ctx_name()));
  auto if_exit = llvm::BasicBlock::Create(
      *context, "if_exit", module->getFunction(cur_scope.get_scope_ctx_name()));

  auto if_cmp = to_operation(visit(ctx->if_br->cmp));
  if (if_cmp->resolve_with_type_info(typesafety::TypeInformation::BOOL_T())) {
    auto cmp_value = if_cmp->generate_value();
    builder->CreateCondBr(cmp_value.get_llvm_val(), if_true, if_false);

    builder->SetInsertPoint(if_true);
    visit(ctx->if_br->body);
    builder->CreateBr(if_exit);

    builder->SetInsertPoint(if_false);
    for (auto* else_if_br : ctx->else_if_brs) {
      auto else_if_true = llvm::BasicBlock::Create(*context, "else_if_true",
                                                   module->getFunction("main"));
      auto else_if_false = llvm::BasicBlock::Create(
          *context, "else_if_false", module->getFunction("main"));

      auto else_if_cmp = to_operation(visit(else_if_br->cmp));
      if (else_if_cmp->resolve_with_type_info(
              typesafety::TypeInformation::BOOL_T())) {
        auto else_if_cmp_value = else_if_cmp->generate_value();
        builder->CreateCondBr(else_if_cmp_value.get_llvm_val(), else_if_true,
                              else_if_false);

        builder->SetInsertPoint(else_if_true);
        visit(else_if_br->body);
        builder->CreateBr(if_exit);
        builder->SetInsertPoint(else_if_false);
      }
    }

    auto else_case = llvm::BasicBlock::Create(*context, "else_case",
                                              module->getFunction("main"));
    if (ctx->else_br) {
      builder->CreateBr(else_case);

      builder->SetInsertPoint(else_case);
      visit(ctx->else_br->body);
    }

    if_exit->moveAfter(else_case);
    builder->CreateBr(if_exit);
    builder->SetInsertPoint(if_exit);
  }

  --*logger;
  return std::any();
}

std::any YALLLVisitorImpl::visitIf(YALLLParser::IfContext* ctx) {
  logger->send_internal_error("Visited if instead of if_else");
  return std::any();
}

std::any YALLLVisitorImpl::visitElse_if(YALLLParser::Else_ifContext* ctx) {
  logger->send_internal_error("Visited else if instead of if_else");
  return std::any();
}

std::any YALLLVisitorImpl::visitElse(YALLLParser::ElseContext* ctx) {
  logger->send_internal_error("Visited else instead of if_else");
  return std::any();
}

std::any YALLLVisitorImpl::visitOperation(YALLLParser::OperationContext* ctx) {
  logger->send_log("Visiting operation");
  ++*logger;
  auto res = visitChildren(ctx);
  --*logger;
  return res;
}

std::any YALLLVisitorImpl::visitReterr_op(YALLLParser::Reterr_opContext* ctx) {
  logger->send_log("Visiting reterr");
  ++*logger;
  auto res = visitChildren(ctx);
  --*logger;
  return res;
}

std::any YALLLVisitorImpl::visitBool_or_op(
    YALLLParser::Bool_or_opContext* ctx) {
  logger->send_log("Visiting or");
  ++*logger;

  if (ctx->rhs.size() == 0) {
    --*logger;
    return visit(ctx->lhs);
  }
  std::vector<std::shared_ptr<yalll::Operation>> operations;
  std::vector<size_t> op_codes;

  operations.push_back(to_operation(visit(ctx->lhs)));

  for (auto* rhs : ctx->rhs) {
    operations.push_back(to_operation(visit(rhs)));
    op_codes.push_back(ctx->op->getType());
  }

  --*logger;
  return std::make_shared<yalll::OrOperation>(operations, op_codes);
}

std::any YALLLVisitorImpl::visitBool_and_op(
    YALLLParser::Bool_and_opContext* ctx) {
  logger->send_log("Visiting and");
  ++*logger;
  if (ctx->rhs.size() == 0) {
    --*logger;
    return visit(ctx->lhs);
  }

  std::vector<std::shared_ptr<yalll::Operation>> operations;
  std::vector<size_t> op_codes;

  operations.push_back(to_operation(visit(ctx->lhs)));

  for (auto* rhs : ctx->rhs) {
    operations.push_back(to_operation(visit(rhs)));
    op_codes.push_back(ctx->op->getType());
  }

  --*logger;
  return std::make_shared<yalll::AndOperation>(operations, op_codes);
}

std::any YALLLVisitorImpl::visitCompare_op(
    YALLLParser::Compare_opContext* ctx) {
  logger->send_log("Visiting cmp");
  ++*logger;
  if (ctx->rhs.size() == 0) {
    --*logger;
    return visit(ctx->lhs);
  }
  std::vector<std::shared_ptr<yalll::Operation>> operations;
  std::vector<size_t> op_codes;

  operations.push_back(to_operation(visit(ctx->lhs)));

  for (auto i = 0; i < ctx->op.size(); ++i) {
    operations.push_back(to_operation(visit(ctx->rhs.at(i))));
    op_codes.push_back(ctx->op.at(i)->getStart()->getType());
  }

  --*logger;
  return std::make_shared<yalll::CmpOperation>(operations, op_codes);
}

std::any YALLLVisitorImpl::visitAddition_op(
    YALLLParser::Addition_opContext* ctx) {
  logger->send_log("Visiting add");
  ++*logger;
  if (ctx->rhs.size() == 0) {
    --*logger;
    return visit(ctx->lhs);
  }

  std::vector<std::shared_ptr<yalll::Operation>> operations;
  std::vector<size_t> op_codes;

  operations.push_back(to_operation(visit(ctx->lhs)));

  for (auto i = 0; i < ctx->op.size(); ++i) {
    operations.push_back(to_operation(visit(ctx->rhs.at(i))));
    op_codes.push_back(ctx->op.at(i)->getType());
  }

  --*logger;
  return std::make_shared<yalll::AddOperation>(operations, op_codes);
}

std::any YALLLVisitorImpl::visitMultiplication_op(
    YALLLParser::Multiplication_opContext* ctx) {
  logger->send_log("Visiting mul");
  ++*logger;
  if (ctx->rhs.size() == 0) {
    --*logger;
    return visit(ctx->lhs);
  }

  std::vector<std::shared_ptr<yalll::Operation>> operations;
  std::vector<size_t> op_codes;

  operations.push_back(to_operation(visit(ctx->lhs)));

  for (auto i = 0; i < ctx->op.size(); ++i) {
    operations.push_back(to_operation(visit(ctx->rhs.at(i))));
    op_codes.push_back(ctx->op.at(i)->getType());
  }

  --*logger;
  return std::make_shared<yalll::MulOperation>(operations, op_codes);
}

std::any YALLLVisitorImpl::visitPrimary_op_high_precedence(
    YALLLParser::Primary_op_high_precedenceContext* ctx) {
  logger->send_log("Visiting primary via precedence");
  ++*logger;

  auto tmp = to_operation(visit(ctx->val));

  --*logger;
  return tmp;
}

std::any YALLLVisitorImpl::visitPrimary_op_fc(
    YALLLParser::Primary_op_fcContext* ctx) {
  logger->send_log("Visiting primary via function call");
  ++*logger;
  auto res = to_operation(visit(ctx->val));
  --*logger;
  return res;
}

std::any YALLLVisitorImpl::visitFunction_call(
    YALLLParser::Function_callContext* ctx) {
  std::string name = ctx->name->getText();
  logger->send_log("Visiting function {} call", name);
  ++*logger;

  auto* func = cur_scope.find_function(name);
  auto arguments =
      std::any_cast<std::vector<std::shared_ptr<yalll::Operation>>>(
          visit(ctx->args));

  --*logger;
  return std::make_shared<yalll::FuncCallOperation>(
      yalll::FuncCallOperation(*func, arguments));
}

std::any YALLLVisitorImpl::visitArgument_list(
    YALLLParser::Argument_listContext* ctx) {
  logger->send_log("Visiting argument list");
  ++*logger;

  std::vector<std::shared_ptr<yalll::Operation>> arguments;
  if (ctx->first_arg) {
    logger->send_log("Arg:{}", ctx->first_arg->getText());
    arguments.push_back(to_operation(visit(ctx->first_arg)));

    for (auto* arg : ctx->nth_arg) {
      logger->send_log("Arg:{}", arg->getText());
      arguments.push_back(to_operation(visit(arg)));
    }
  }

  --*logger;
  return std::move(arguments);
}

std::any YALLLVisitorImpl::visitPrimary_op_term(
    YALLLParser::Primary_op_termContext* ctx) {
  logger->send_log("Visiting primary via terminal");
  ++*logger;
  auto res = to_operation(visit(ctx->val));
  --*logger;
  return res;
}

std::any YALLLVisitorImpl::visitTerminal_op(
    YALLLParser::Terminal_opContext* ctx) {
  logger->send_log("Visiting terminal");
  ++*logger;
  logger->send_log("Value: {}", ctx->getText());
  switch (ctx->val->getType()) {
    case YALLLParser::INTEGER:
      logger->send_log("Integer");
      --*logger;
      return std::make_shared<yalll::TerminalOperation>(
          yalll::Value(typesafety::TypeInformation::INTAUTO_T(),
                       ctx->val->getText(), ctx->val->getLine()));

    case YALLLParser::NAME: {
      auto* value = cur_scope.find_field(ctx->val->getText());
      logger->send_log("{}", value->to_string());
      if (value) {
        --*logger;
        return std::make_shared<yalll::TerminalOperation>(*value);
      } else {
        logger->send_error("Undefined variable {} used inline {}",
                           ctx->val->getText(), ctx->val->getLine());
        --*logger;
        return std::make_shared<yalll::TerminalOperation>(yalll::Value(
            typesafety::TypeInformation::VOID_T(),
            llvm::PoisonValue::get(builder->getVoidTy()), ctx->val->getLine()));
      }
    }

    case YALLLParser::DECIMAL:
      logger->send_log("Decimal");
      --*logger;
      return std::make_shared<yalll::TerminalOperation>(
          yalll::Value(typesafety::TypeInformation::DECAUTO_T(),
                       ctx->val->getText(), ctx->val->getLine()));

    case YALLLParser::BOOL_TRUE:
      logger->send_log("Bool");
      --*logger;
      return std::make_shared<yalll::TerminalOperation>(
          yalll::Value(typesafety::TypeInformation::BOOL_T(),
                       builder->getInt1(true), ctx->val->getLine()));

    case YALLLParser::BOOL_FALSE:
      logger->send_log("Bool");
      --*logger;
      return std::make_shared<yalll::TerminalOperation>(
          yalll::Value(typesafety::TypeInformation::BOOL_T(),
                       builder->getInt1(false), ctx->val->getLine()));

    case YALLLParser::NULL_VALUE:
      logger->send_log("Null");
      --*logger;
      return std::make_shared<yalll::TerminalOperation>(
          yalll::Value::NULL_VALUE(ctx->val->getLine()));

    default:
      logger->send_error("Unkonw terminal type {} found in line {}",
                         ctx->val->getText(), ctx->val->getLine());

      --*logger;
      return std::any();
      break;
  }
}

}  // namespace yallc
