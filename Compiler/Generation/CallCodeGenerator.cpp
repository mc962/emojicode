//
//  CallCodeGenerator.cpp
//  Emojicode
//
//  Created by Theo Weidmann on 05/08/2017.
//  Copyright © 2017 Theo Weidmann. All rights reserved.
//

#include "CallCodeGenerator.hpp"
#include "AST/ASTExpr.hpp"
#include "FunctionCodeGenerator.hpp"
#include "Functions/Initializer.hpp"
#include "Types/Protocol.hpp"
#include "Types/TypeDefinition.hpp"
#include "Generation/TypeDescriptionGenerator.hpp"
#include <llvm/Support/raw_ostream.h>
#include <stdexcept>

namespace EmojicodeCompiler {

CallCodeGenerator::CallCodeGenerator(FunctionCodeGenerator *fg, CallType callType) : fg_(fg), callType_(callType) {}
CallCodeGenerator::~CallCodeGenerator() = default;

llvm::Value *CallCodeGenerator::generate(llvm::Value *callee, const Type &type, const ASTArguments &astArgs,
                                         Function *function, llvm::Value *errorPointer,
                                         const std::vector<llvm::Value *> &supplArgs) {
    auto args = createArgsVector(callee, astArgs, errorPointer, supplArgs);

    assert(function != nullptr);
    switch (callType_) {
        case CallType::StaticContextfreeDispatch:
        case CallType::StaticDispatch: {
            auto llvmFn = function->reificationFor(astArgs.genericArgumentTypes()).function;
            llvm::Type *castTo = nullptr;
            if (!args.empty() && args.front()->getType() != llvmFn->args().begin()->getType()) {
                if (function->functionType() == FunctionType::ObjectInitializer) {
                    castTo = args.front()->getType();
                }
                args.front() = fg()->builder().CreateBitCast(args.front(), llvmFn->args().begin()->getType());
            }
            auto ret = fg_->builder().CreateCall(llvmFn, args);
            return castTo == nullptr ? ret : fg_->builder().CreateBitCast(ret, castTo);
        }
        case CallType::DynamicDispatch:
        case CallType::DynamicDispatchOnType:
            assert(type.type() == TypeType::Class);
            return createDynamicDispatch(function, args, astArgs.genericArgumentTypes());
        case CallType::DynamicProtocolDispatch: {
            assert(type.type() == TypeType::Box);

            llvm::Value *conformance;
            if (type.boxedFor().type() != TypeType::Protocol) {
                conformance = buildFindProtocolConformance(args, type.unboxed());
            }
            else {
                auto conformanceType = fg()->typeHelper().protocolConformance();
                auto ptr = fg()->buildGetBoxInfoPtr(args.front());
                conformance = fg()->builder().CreateLoad(llvm::PointerType::getUnqual(fg()->ctx()), ptr);
            }
            return createDynamicProtocolDispatch(function, args, astArgs.genericArgumentTypes(), conformance);
        }
        case CallType::None:
            throw std::domain_error("CallType::None is not a valid call type");
    }
    if (tdg_ != nullptr) {
        tdg_->restoreStack();
    }
}

llvm::Value* CallCodeGenerator::buildFindProtocolConformance(const std::vector<llvm::Value *> &args,
                                                             const Type &protocol) {
    auto boxInfoGep73 = fg()->buildGetBoxInfoPtr(args.front());
    auto boxInfo = fg()->builder().CreateLoad(llvm::PointerType::getUnqual(fg()->ctx()), boxInfoGep73);
    return fg()->buildFindProtocolConformance(args.front(), boxInfo, protocol.protocol()->rtti());
}

std::vector<Value *> CallCodeGenerator::createArgsVector(llvm::Value *callee, const ASTArguments &args,
                                                         llvm::Value *errorPointer,
                                                         const std::vector<llvm::Value *> &supplArgs) {
    std::vector<Value *> argsVector;
    if (callType_ != CallType::StaticContextfreeDispatch) {
        argsVector.emplace_back(callee);
    }
    for (auto &arg : args.args()) {
        argsVector.emplace_back(arg->generate(fg_));
    }
    argsVector.insert(argsVector.end(), supplArgs.begin(), supplArgs.end());
    if (!args.genericArguments().empty()) {
        tdg_ = std::make_unique<TypeDescriptionGenerator>(fg_, TypeDescriptionUser::Function);
        argsVector.emplace_back(tdg_->generate(args.genericArguments()));
    }
    if (errorPointer != nullptr) {
        argsVector.emplace_back(errorPointer);
    }
    return argsVector;
}

llvm::Value *MultiprotocolCallCodeGenerator::generate(llvm::Value *callee, const Type &calleeType,
                                                      const ASTArguments &args, Function* function,
                                                      llvm::Value *errorPointer, size_t multiprotocolN) {
    assert(calleeType.type() == TypeType::Box);
    assert(function != nullptr);

    auto argsv = createArgsVector(callee, args, errorPointer, {});

    llvm::Value *conformance;
    if (calleeType.boxedFor().type() != TypeType::MultiProtocol) {
        conformance = buildFindProtocolConformance(argsv, calleeType.protocols()[multiprotocolN]);
    }
    else {
        auto mpt = fg()->typeHelper().multiprotocolConformance(calleeType);
        auto mp = fg()->buildGetBoxInfoPtr(argsv.front());
        auto mpl = fg()->builder().CreateLoad(llvm::PointerType::getUnqual(fg()->ctx()), mp);

        auto mpGep = fg()->builder().CreateConstGEP2_32(mpt, mpl, 0, multiprotocolN);
        conformance = fg()->builder().CreateLoad(llvm::PointerType::getUnqual(fg()->ctx()), mpGep);
    }
    return createDynamicProtocolDispatch(function, std::move(argsv), args.genericArgumentTypes(), conformance);
}

llvm::Value *CallCodeGenerator::dispatchFromVirtualTable(Function *function, llvm::Value *virtualTable,
                                                         const std::vector<llvm::Value *> &args,
                                                         const std::vector<Type> &genericArguments) {
    auto reification = function->reificationFor(genericArguments);
    auto id = fg()->int32(reification.vti());
    auto vtGep = fg()->builder().CreateInBoundsGEP(llvm::PointerType::getUnqual(fg()->ctx()), virtualTable, id);
    auto dispatchedFunc = fg()->builder().CreateLoad(llvm::PointerType::getUnqual(fg()->ctx()), vtGep);

    std::vector<llvm::Type *> argTypes = reification.functionType()->params();
    if (callType_ == CallType::DynamicProtocolDispatch) {
        argTypes.front() = llvm::PointerType::getUnqual(fg()->ctx());
    }
    else if (callType_ == CallType::DynamicDispatch) {
        argTypes.front() = args.front()->getType();
    }
    else if (callType_ == CallType::DynamicDispatchOnType) {
        assert(argTypes.front() == args.front()->getType());
    }

    auto funcType = llvm::FunctionType::get(reification.functionType()->getReturnType(), argTypes, false);
    auto func = fg()->builder().CreateBitCast(dispatchedFunc, funcType->getPointerTo(), "dispatchFunc");
    return fg_->builder().CreateCall(funcType, func, args);
}

llvm::Value *CallCodeGenerator::createDynamicDispatch(Function *function, const std::vector<llvm::Value *> &args,
                                                      const std::vector<Type> &genericArgs) {
    auto info = callType_ == CallType::DynamicDispatchOnType ? args.front() : fg()->buildGetClassInfoFromObject(args.front());
    auto tablePtr = fg()->builder().CreateConstInBoundsGEP2_32(fg_->typeHelper().classInfo(), info, 0, 1);
    auto table = fg()->builder().CreateLoad(llvm::PointerType::getUnqual(fg()->ctx()), tablePtr, "table");
    return dispatchFromVirtualTable(function, table, args, genericArgs);
}

llvm::Value *CallCodeGenerator::createDynamicProtocolDispatch(Function *function, std::vector<llvm::Value *> args,
                                                              const std::vector<Type> &genericArgs,
                                                              llvm::Value *conformance) {
    args.front() = getProtocolCallee(args, conformance);

    auto tableGep = fg()->builder().CreateConstGEP2_32(fg()->typeHelper().protocolConformance(), conformance, 0, 1);
    auto table = fg()->builder().CreateLoad(llvm::PointerType::getUnqual(fg()->ctx()), tableGep, "table");
    return dispatchFromVirtualTable(function, table, args, genericArgs);
}

llvm::Value *CallCodeGenerator::getProtocolCallee(std::vector<Value *> &args, llvm::Value *conformance) const {
    auto shouldLoadPtr = fg()->builder().CreateConstGEP2_32(fg()->typeHelper().protocolConformance(),
                                                            conformance, 0, 0);
    return fg()->createIfElsePhi(fg()->builder().CreateLoad(llvm::Type::getInt1Ty(fg()->ctx()), shouldLoadPtr, "shouldLoad"), [this, &args]() {
        auto value = fg()->buildGetBoxValuePtr(args.front(), llvm::PointerType::getUnqual(fg()->ctx())->getPointerTo());
        return fg()->builder().CreateLoad(llvm::PointerType::getUnqual(fg()->ctx()), value);
    }, [this, &args]() {
        return fg()->buildGetBoxValuePtr(args.front(), llvm::PointerType::getUnqual(fg()->ctx()));
    });
}

}  // namespace EmojicodeCompiler
