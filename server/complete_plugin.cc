/*

g++ -c complete_plugin.cc \
    `~/src/llvm-svn/Release+Asserts/bin/llvm-config --cxxflags` \
    -I/Users/thakis/src/llvm-svn/tools/clang/include

g++ -dynamiclib -Wl,-undefined,dynamic_lookup \
    complete_plugin.o -o libcomplete_plugin.dylib

*/
#include "clang/Frontend/FrontendPluginRegistry.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/AST.h"
#include "clang/AST/TypeLoc.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/CompilerInstance.h"
using namespace clang;


class CompletePlugin : public ASTConsumer {
public:
  CompletePlugin(CompilerInstance& instance)
      : instance_(instance),
        d_(instance.getDiagnostics()) {
  }

  virtual void HandleTagDeclDefinition(TagDecl* tag) {
    if (CXXRecordDecl* record = dyn_cast<CXXRecordDecl>(tag)) {
      SourceLocation record_location = record->getInnerLocStart();
      record->getIdentifier();
    }
  }

  virtual void HandleTopLevelDecl(DeclGroupRef D) {
  }

private:
  CompilerInstance& instance_;
  Diagnostic& d_;
};


class CompletePluginAction : public PluginASTAction {
 protected:
  ASTConsumer* CreateASTConsumer(CompilerInstance &CI, llvm::StringRef ref) {
    return new CompletePlugin(CI);
  }

  bool ParseArgs(const CompilerInstance &CI,
                 const std::vector<std::string>& args) {
    return true;
  }
};


static FrontendPluginRegistry::Add<CompletePluginAction>
X("complete", "Adds information about symbols to a sqlite database.");
