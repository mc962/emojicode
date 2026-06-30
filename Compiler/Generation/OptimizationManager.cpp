//
// Created by Theo Weidmann on 25.03.18.
//

#include "OptimizationManager.hpp"
#include "ReferenceCountingPasses.hpp"
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Transforms/IPO.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/Scalar/GVN.h>
#include <llvm/Transforms/Utils.h>

namespace EmojicodeCompiler {

OptimizationManager::OptimizationManager(llvm::Module *module, bool optimize, RunTimeHelper *runTime)
        : optimize_(optimize), functionPassManager_(std::make_unique<llvm::legacy::FunctionPassManager>(module)),
            passManager_(std::make_unique<llvm::legacy::PassManager>()) {
                initialize(runTime);
            }

void OptimizationManager::initialize(RunTimeHelper *runTime) {
    if (optimize_) {
        passManager_->add(new LocalReferenceCountingPass(runTime));

        // Function-level passes (O3 subset)
        functionPassManager_->add(llvm::createPromoteMemoryToRegisterPass());
        functionPassManager_->add(llvm::createSROAPass());
        functionPassManager_->add(llvm::createEarlyCSEPass(true));
        functionPassManager_->add(llvm::createCFGSimplificationPass());
        functionPassManager_->add(llvm::createReassociatePass());
        functionPassManager_->add(llvm::createGVNPass());
        functionPassManager_->add(llvm::createDeadCodeEliminationPass());
        functionPassManager_->add(llvm::createTailCallEliminationPass());
        functionPassManager_->add(llvm::createLICMPass());
        functionPassManager_->doInitialization();

        // Module-level passes
        passManager_->add(llvm::createDeadArgEliminationPass());

        passManager_->add(new ConstantReferenceCountingPass(runTime));
        passManager_->add(new RedundantReferenceCountingPass(runTime));
    }
}

void OptimizationManager::optimize(llvm::Function *function) {
    if (optimize_) {
        functionPassManager_->run(*function);
    }
}

void OptimizationManager::optimize(llvm::Module *module) {
    if (optimize_) {
        passManager_->run(*module);
    }
}

}  // namespace EmojicodeCompiler
