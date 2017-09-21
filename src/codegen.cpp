#include "codegen.h"
#include <iostream>

#include <llvm/ADT/APFloat.h>
#include <llvm/IR/Verifier.h>

namespace babycpp {
namespace codegen {

using llvm::Value;

const std::unordered_map<int, int> Codegenerator::AST_LLVM_MAP{
    {Token::tok_float, llvm::Type::TypeID::FloatTyID},
    {Token::tok_int, llvm::Type::TypeID::IntegerTyID},
};

inline llvm::Type *getType(int type, Codegenerator *gen) {
  if (type == Token::tok_float) {
    return llvm::Type::getFloatTy(gen->context);
  } else {
    return llvm::Type::getInt32Ty(gen->context);
  }
}
llvm::AllocaInst *
Codegenerator::createEntryBlockAlloca(llvm::Function *function,
                                      const std::string &varName, int type) {
  llvm::IRBuilder<> tempBuilder(&function->getEntryBlock(),
                                function->getEntryBlock().begin());
  llvm::Type *varType = getType(type, this);
  return tempBuilder.CreateAlloca(varType, 0, varName.c_str());
}

Value *NumberExprAST::codegen(Codegenerator *gen) {

  if (val.type == Token::tok_float) {
    return llvm::ConstantFP::get(gen->context, llvm::APFloat(val.floatNumber));

  } else if (val.type == Token::tok_int) {
    return llvm::ConstantInt::get(gen->context,
                                  llvm::APInt(32, val.integerNumber));
  }
  // this should not be triggered, we should find this errors at
  // parsing time
  std::cout << "Error unrecognized type number on code gen" << std::endl;
  return nullptr;
}
Codegenerator::Codegenerator()
    : lexer(), parser(&lexer), context(), builder(context),
      module("", context) {}

llvm::Value *VariableExprAST::codegen(Codegenerator *gen) {

  // first we try to see if the variable is already defined at scope
  // level
  llvm::AllocaInst *v = gen->namedValues[name];
  // here we extract the variable from the scope.
  // if we get a nullptr and the variable is not a definition
  // we got an error
  if (v == nullptr && datatype == 0) {
    std::cout << "Error variable " << name << " not defined" << std::endl;
    return nullptr;
  }

  if (flags.isDefinition == true) {
    llvm::IRBuilder<> tempBuilder(&gen->currentScope->getEntryBlock(),
                                  gen->currentScope->getEntryBlock().begin());
    llvm::Type *varType = getType(datatype, gen);
    v = tempBuilder.CreateAlloca(varType, 0, name.c_str());
    gen->namedValues[name.c_str()] = v;

    if (value == nullptr) {
      std::cout << "error: expected value for value definition" << std::endl;
      return nullptr;
    }

    Value *valGen = value->codegen(gen);
    // return gen->builder.CreateLoad(v, name.c_str());
    return gen->builder.CreateStore(valGen, v);
  } else if (datatype == 0) {
    if (v->getAllocatedType()->getTypeID() == llvm::Type::FloatTyID) {
      datatype = Token::tok_float;
    } else {
      datatype = Token::tok_int;
    }

    // now at this point ,we might have a simple variable for
    // which we gen a load, or we might have an assigment
    // if it is the case ,we have a value which is not nullptr
    if (value != nullptr) {
      Value *valGen = value->codegen(gen);
      // return gen->builder.CreateLoad(v, name.c_str());
      return gen->builder.CreateStore(valGen, v);
    } else {
      // generating a load
      return gen->builder.CreateLoad(v, name.c_str());
    }
  } else {
    // if we got here, it means the variable has a known datatype
    // but has not been defined yet, this only happens for variable
    // definitions, so we need to define it, we are gonna do that with
    // alloca
    std::cout << "not definition " << name << std::endl;
    return nullptr;
  }

  return nullptr;
}

int Codegenerator::omogenizeOperation(ExprAST *L, ExprAST *R,
                                      llvm::Value **Lvalue,
                                      llvm::Value **Rvalue) {

  int Ltype = L->datatype;
  int Rtype = R->datatype;

  if (Ltype == 0 || Rtype == 0) {
    std::cout << "error cannot deduce output type of operation" << std::endl;
    return -1;
  }

  if (Ltype == Rtype) {
    // same type nothing to do here
    return Ltype;
  }

  if (Ltype == Token::tok_float && Rtype == Token::tok_int) {
    // need to convert R side
    *Rvalue = builder.CreateUIToFP(*Rvalue, llvm::Type::getFloatTy(context),
                                   "intToFPcast");
    // TODO(giordi) implement wraning log
    // std::cout << "warning: implicit conversion int->float" << std::endl;
    return Token::tok_float;
  } else if (Rtype == Token::tok_float && Ltype == Token::tok_int) {
    // need to convert L side
    *Lvalue = builder.CreateUIToFP(*Lvalue, llvm::Type::getFloatTy(context),
                                   "intToFPcast");
    // TODO(giordi) implement wraning log
    // std::cout << "warning: implicit conversion int->float" << std::endl;
    return Token::tok_float;
  }

  // should never reach this
  return -1;
}
llvm::Value *BinaryExprAST::codegen(Codegenerator *gen) {
  // generating code recursively for left and right end side
  Value *L = lhs->codegen(gen);
  Value *R = rhs->codegen(gen);

  if (L == nullptr || R == nullptr) {
    return nullptr;
  }

  datatype = gen->omogenizeOperation(lhs, rhs, &L, &R);

  // checking the operator to generate the correct operation
  if (op == "+") {
    return gen->builder.CreateFAdd(L, R, "addtmp");
  } else if (op == "-") {
    return gen->builder.CreateFSub(L, R, "subtmp");
  } else if (op == "*") {
    return gen->builder.CreateFMul(L, R, "multmp");
  } else if (op == "/") {
    return gen->builder.CreateFDiv(L, R, "divtmp");
  } else if (op == "<") {
    // TODO(giordi) fix this, to return int?
    L = gen->builder.CreateFCmpULT(L, R, "cmptmp");
    return gen->builder.CreateUIToFP(L, llvm::Type::getDoubleTy(gen->context),
                                     "booltmp");
  }
  std::cout << "error unrecognized operator" << std::endl;
  return nullptr;
}

llvm::Value *PrototypeAST::codegen(Codegenerator *gen) {
  uint32_t argSize = args.size();
  std::vector<llvm::Type *> funcArgs(argSize);
  // generating args with correct type
  for (uint32_t t = 0; t < argSize; ++t) {
    const auto &astArg = args[t];
    funcArgs[t] = getType(astArg.type, gen);
  }

  llvm::Type *returnType = getType(datatype, gen);
  auto *funcType = llvm::FunctionType::get(returnType, funcArgs, false);

  auto *function = llvm::Function::Create(
      funcType, llvm::Function::ExternalLinkage, name, &gen->module);
  // Set names for all arguments.
  uint32_t Idx = 0;
  for (auto &arg : function->args())
    arg.setName(args[Idx++].name);

  return function;
}

llvm::Value *FunctionAST::codegen(Codegenerator *gen) {
  //// First, check for an existing function from a previous 'extern'
  /// declaration.
  llvm::Function *function = gen->module.getFunction(proto->name);

  if (function == nullptr) {
    Value *p = proto->codegen(gen);
    function = static_cast<llvm::Function *>(p);
  }
  if (function == nullptr) {
    std::cout << "error generating protoype code gen" << std::endl;
    return nullptr;
  }

  if (!function->empty()) {
    std::cout << "Function cannot be redefined." << std::endl;
    return nullptr;
  }

  using llvm::BasicBlock;
  // Create a new basic block to start insertion into.
  BasicBlock *block = BasicBlock::Create(gen->context, "entry", function);
  gen->builder.SetInsertPoint(block);

  // Record the function arguments in the NamedValues map.
  gen->namedValues.clear();
  int counter = 0;
  for (auto &arg : function->args()) {
    // Create an alloca for this variable.
    llvm::AllocaInst *alloca = gen->createEntryBlockAlloca(
        function, arg.getName(), proto->args[counter++].type);

    // Store the initial value into the alloca.
    gen->builder.CreateStore(&arg, alloca);

    // Add arguments to variable symbol table.
    gen->namedValues[arg.getName()] = alloca;
  }

  gen->currentScope = function;
  for (auto &b : body) {
    if (Value *RetVal = b->codegen(gen)) {
      if (b->flags.isReturn) {
        gen->builder.CreateRet(RetVal);
      }
    }
  }
  gen->currentScope = nullptr;

  std::string outs;
  llvm::raw_string_ostream os(outs);
  bool res = verifyFunction(*function, &os);
  if (res) {
    os.flush();
    std::cout << "error verifying function" << outs << std::endl;
    std::cout << "here what was generated" << std::endl;
    // std::string outs;
    // llvm::raw_string_ostream os(outs);
    // gen->printLlvmData(function);
    gen->module.print(llvm::errs(), nullptr);
    return nullptr;
  }
  return function;
}

bool Codegenerator::compareASTArgWithLLVMArg(ExprAST *astArg,
                                             llvm::Argument *llvmArg) {
  auto found = AST_LLVM_MAP.find(astArg->datatype);
  if (found != AST_LLVM_MAP.end()) {
    if (found->second == llvmArg->getType()->getTypeID()) {
      return true;
    }
    return false;
  }
  return false;
}

llvm::Value *CallExprAST::codegen(Codegenerator *gen) {
  // lests try to get the function
  llvm::Function *calleeF = gen->module.getFunction(callee);
  if (calleeF == nullptr) {
    std::cout << "error function not defined" << std::endl;
    return nullptr;
  }

  // checking function signature
  if (calleeF->arg_size() != args.size()) {
    std::cout << "error function call with wrong number of args" << std::endl;
    return nullptr;
  }
  uint32_t argSize = args.size();
  std::vector<Value *> argValues;
  argValues.reserve(argSize);
  for (uint32_t t = 0; t < argSize; ++t) {
    // check type
    llvm::Argument *currFunctionArg = calleeF->args().begin() + t;
    if (!Codegenerator::compareASTArgWithLLVMArg(args[t], currFunctionArg)) {
      std::cout << "mismatch type for function call argument" << std::endl;
      return nullptr;
    }
    // if we got here the type is correct, so we can push the argument
    Value *argValuePtr = args[t]->codegen(gen);
    if (argValuePtr == nullptr) {
      std::cout << "error in genrating code for function argument" << std::endl;
      return nullptr;
    }
    argValues.push_back(argValuePtr);
  }

  return gen->builder.CreateCall(calleeF, argValues, "calltmp");
}

} // namespace codegen
} // namespace babycpp