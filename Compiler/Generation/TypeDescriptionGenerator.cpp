//
//  TypeDescriptionGenerator.cpp
//  runtime
//
//  Created by Theo Weidmann on 25.03.19.
//

#include "TypeDescriptionGenerator.hpp"
#include <llvm/Support/Alignment.h>
#include "Generation/LLVMTypeHelper.hpp"
#include "Generation/FunctionCodeGenerator.hpp"
#include "Types/Class.hpp"
#include "Types/ValueType.hpp"
#include "Types/Protocol.hpp"
#include "Compiler.hpp"
#include "Generation/RunTimeHelper.hpp"

namespace EmojicodeCompiler {

void TypeDescriptionGenerator::addType(const Type &type) {
    llvm::Constant *genericInfo;
    auto notype = type.unoptionalized().unboxed();
    switch (notype.type()) {
        case TypeType::Class:
            genericInfo = buildConstant00Gep(fg_->typeHelper().classInfo(), notype.klass()->classInfo(), fg_->ctx());
            break;
        case TypeType::Protocol:
            genericInfo = notype.protocol()->rtti();
            break;
        case TypeType::ValueType:
        case TypeType::Enum:
            genericInfo = buildConstant00Gep(fg_->typeHelper().boxInfo(), fg_->boxInfoFor(notype), fg_->ctx());
            break;
        case TypeType::Something:
            genericInfo = fg_->generator()->runTime().somethingRtti();
            break;
        case TypeType::Someobject:
            genericInfo = fg_->generator()->runTime().someobjectRtti();
            break;
        case TypeType::GenericVariable:
            if (!fg_->calleeType().is<TypeType::TypeAsValue>() &&
                fg_->calleeType().typeDefinition()->isGenericDynamismDisabled()) {
                throw CompilerError(fg_->position(), "Generic dynamism is disabled in this type.");
            }
            addDynamic(extractTypeDescriptionPtr(), notype.genericVariableIndex());
            return;
        case TypeType::LocalGenericVariable:
            addDynamic(fg_->functionGenericArgs(), notype.genericVariableIndex());
            return;
        case TypeType::Callable:
        case TypeType::TypeAsValue:
        case TypeType::MultiProtocol:
            genericInfo = fg_->generator()->runTime().somethingRtti();
            fg_->compiler()->warn(SourcePosition(), "Run-time type information for multiprotocols, callables and type "\
                                  "values is not available yet. Casts and other reflection may not behave as "\
                                  "expected with these types.");
            break;
        default:
            throw std::logic_error("Cannot create type description for compile-time type.");
    }

    auto strct = llvm::ConstantStruct::get(fg_->typeHelper().typeDescription(), {
        genericInfo,
        type.type() == TypeType::Optional ? llvm::ConstantInt::getTrue(fg_->ctx()) : llvm::ConstantInt::getFalse(fg_->ctx())
    });
    types_.emplace_back(strct);

    if (!notype.canHaveGenericArguments()) return;
    for (auto &arg : notype.genericArguments()) {
        addType(arg);
    }
}

llvm::Value* TypeDescriptionGenerator::extractTypeDescriptionPtr() {
    if (fg_->calleeType().is<TypeType::TypeAsValue>()) {
        return fg_->genericArgsPtr();
    }
    auto gaptrPtr77 = fg_->genericArgsPtr();
    auto gasType = fg_->typeHelper().genericArgsStore(fg_->calleeType());
    auto ptr = fg_->builder().CreateLoad(gasType, gaptrPtr77);
    if (fg_->calleeType().is<TypeType::Class>()) {
        // gasType is {typeDescription_*, i1}; extract field 0 to get typeDescription_*
        return fg_->builder().CreateExtractValue(ptr, 0);
    }
    // For ValueType: gasType is managable(typeDescription_)* — ptr is already the managable ptr
    auto mngType = fg_->typeHelper().managable(fg_->typeHelper().typeDescription());
    return fg_->builder().CreateConstInBoundsGEP2_32(mngType, ptr, 0, 1);
}

void TypeDescriptionGenerator::addDynamic(llvm::Value *gargs, size_t index) {
    dynamic_++;
    auto idf = fg_->generator()->runTime().indexTypeDescription();
    auto td = index > 0 ? fg_->builder().CreateCall(idf, { gargs, fg_->int64(index) }) : gargs;
    auto size = fg_->builder().CreateCall(fg_->generator()->runTime().typeDescriptionLength(), td);
    types_.emplace_back(td, size);
}

llvm::Value* TypeDescriptionGenerator::generate(const std::vector<Type> &types) {
    assert(types_.empty());
    for (auto &type : types) {
        addType(type);
    }
    return finish();
}

llvm::Value* TypeDescriptionGenerator::generate(const std::vector<std::shared_ptr<ASTType>> &types) {
    assert(types_.empty());
    for (auto &type : types) {
        addType(type->type());
    }
    return finish();
}

llvm::Value* TypeDescriptionGenerator::generate(const Type &type) {
    assert(types_.empty());
    addType(type);
    return finish();
}

llvm::Value* TypeDescriptionGenerator::finish() {
    if (dynamic_ == 0) return finishStatic();

    llvm::Value *size = fg_->int64(types_.size() - dynamic_);
    for (auto &tdv : types_) {
        if (tdv.isCopy()) {
            size = fg_->builder().CreateAdd(size, tdv.size);
        }
    }

    llvm::Value *current, *alloc;
    auto typeDesc = fg_->typeHelper().typeDescription();
    if (user_ == User::Function) {
        stack_ = fg_->builder().CreateStackSave();
        current = alloc = fg_->builder().CreateAlloca(typeDesc, size);
    }
    else {
        auto allocSize = fg_->builder().CreateMul(fg_->sizeOf(typeDesc), size);
        if (user_ == User::Class) {
            current = alloc = fg_->builder().CreateCall(fg_->generator()->runTime().malloc(), allocSize);
        }
        else {
            auto mngType = fg_->typeHelper().managable(fg_->typeHelper().typeDescription());
            auto size = fg_->builder().CreateAdd(fg_->sizeOf(llvm::PointerType::getUnqual(fg_->ctx())), allocSize);
            alloc = fg_->builder().CreateCall(fg_->generator()->runTime().alloc(), size);
            current = fg_->builder().CreateConstInBoundsGEP2_32(mngType, alloc, 0, 1);
        }
    }

    for (auto &tdv : types_) {
        if (tdv.isCopy()) {
            fg_->builder().CreateMemCpy(current, llvm::MaybeAlign(), tdv.from, llvm::MaybeAlign(), fg_->builder().CreateMul(fg_->sizeOf(typeDesc), tdv.size));
            current = fg_->builder().CreateInBoundsGEP(typeDesc, current, tdv.size);
        }
        else {
            fg_->builder().CreateStore(tdv.concrete, current);
            current = fg_->builder().CreateConstInBoundsGEP1_32(typeDesc, current, 1);
        }
    }
    if (user_ == User::Class) {
        auto ptrTy = llvm::PointerType::getUnqual(fg_->ctx());
        auto sct = llvm::ConstantStruct::getAnon({ llvm::UndefValue::get(ptrTy),
            llvm::ConstantInt::getFalse(fg_->ctx()) });
        return fg_->builder().CreateInsertValue(sct, alloc, { 0 });
    }
    return alloc;
}

llvm::Value* TypeDescriptionGenerator::finishStatic() {
    auto typeDesc = fg_->typeHelper().typeDescription();
    auto type = llvm::ArrayType::get(typeDesc, types_.size());
    std::vector<llvm::Constant*> cargs;
    for (auto &arg : types_) {
        cargs.emplace_back(arg.concrete);
    }

    llvm::Constant *init = llvm::ConstantArray::get(type, cargs);
    if (user_ == User::ValueTypeOrValue) {
        init = llvm::ConstantStruct::getAnon({ fg_->generator()->runTime().ignoreBlockPtr(), init });
    }
    auto var = new llvm::GlobalVariable(*fg_->generator()->module(), init->getType(), true,
                                        llvm::GlobalValue::LinkageTypes::PrivateLinkage, init);
    var->setUnnamedAddr(llvm::GlobalVariable::UnnamedAddr::Global);

    if (user_ == User::ValueTypeOrValue) {
        return fg_->builder().CreateBitCast(var, llvm::PointerType::getUnqual(fg_->ctx()));
    }
    auto gep = buildConstant00Gep(type, var, fg_->ctx());
    if (user_ == User::Class) {
        return llvm::ConstantStruct::getAnon({ gep, llvm::ConstantInt::getTrue(fg_->ctx()) });
    }
    return gep;
}

void TypeDescriptionGenerator::restoreStack() {
    assert(user_ == User::Function);
    if (stack_ != nullptr) {
        fg_->builder().CreateStackRestore(stack_);
    }
}

}  // namespace EmojicodeCompiler
