/*

g++ -c complete_plugin.cc \
    `~/src/llvm-svn/Release+Asserts/bin/llvm-config --cxxflags` \
    -I/Users/thakis/src/llvm-svn/tools/clang/include

g++ -dynamiclib -Wl,-undefined,dynamic_lookup \
    -lsqlite3 complete_plugin.o -o libcomplete_plugin.dylib

~/src/llvm-svn/Release+Asserts/bin/clang++ -c complete_plugin.cc\
    `~/src/llvm-svn/Release+Asserts/bin/llvm-config --cxxflags` \
    -I/Users/thakis/src/llvm-svn/tools/clang/include \
    -Xclang -load -Xclang libcomplete_plugin.dylib \
    -Xclang -add-plugin -Xclang complete

The database is meant to be converted into this format later on:
http://ctags.sourceforge.net/FORMAT

  select symbol, name, linenr
  from symbols join filenames on symbols.fileid = filenames.rowid
  where symbol = 'begin';

*/
#include <string>

#include "clang/Frontend/FrontendPluginRegistry.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/AST.h"
#include "clang/AST/TypeLoc.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/CompilerInstance.h"
using namespace clang;

#include "sqlite3.h"

// TODO(thakis): Less hardcoded.
//const char kDbPath[] = "/Users/thakis/src/chrome-git/src/builddb.sqlite";
const char kDbPath[] = "builddb.sqlite";


class StupidDatabase {
public:
  StupidDatabase() : db_(NULL) {}
  ~StupidDatabase() { close(); }

  bool open(const std::string& file) {
    assert(!db_);
    int err = sqlite3_open(file.c_str(), &db_);
    return err == SQLITE_OK;
  }

  bool exec(const std::string& query) {
    if (!db_)
      return false;
    return sqlite3_exec(db_, query.c_str(), NULL, NULL, NULL) == SQLITE_OK;
  }

  void close() {
    if (db_)
      sqlite3_close(db_);
    db_ = NULL;
  }

  sqlite3* db() { return db_; }
private:
  sqlite3* db_;
};


class CompletePluginDB {
public:
  bool open(const std::string& file) {
    bool success = db_.open(file);
    if (success) {
      // Putting everything in one transaction is the biggest win. With
      // all these settings, run time goes from 14.4s to 1.9s (compared to
      // 1.6s when running without the plugin).
      db_.exec("pragma synchronous = off");  // 14.4s -> 7.6s
      db_.exec("pragma journal_mode = memory");  // 14.4s -> 6.6s
      //db_.exec("pragma journal_mode = off");  // about the same as "memory"

      prepareTables();

      // Everything in 1 transaction: 14.4s -> 2.6s
      db_.exec("begin transaction");
    }
    return success;
  }

  void flush() {
    db_.exec("end transaction");
  }

  void prepareTables() {
    db_.exec("create table if not exists filenames(name, basename)");
    db_.exec(
        "create index if not exists filename_name_idx on filenames(name)");
    db_.exec(
        "create index if not exists filename_basename_idx "
        "on filenames(basename)");

    db_.exec(
        "create table if not exists symbols "
        "    (fileid integer, linenr integer, symbol, kind, "
        "     primary key(fileid, linenr, symbol))");
  }

  int getFileId(const std::string& file) {
    if (file == lastFile_) return lastFileId_;

    // FIXME: move sqlite-specific stuff into StupidDatabase
    const char query[] = "select rowid from filenames where name=?";
    sqlite3_stmt* query_stmt = NULL;
    if (sqlite3_prepare_v2(
          db_.db(), query, -1, &query_stmt, NULL) != SQLITE_OK) {
      return -1;
    }
    sqlite3_bind_text(query_stmt, 1, file.c_str(), -1, SQLITE_TRANSIENT);

    int rowid = -1;
    int code = sqlite3_step(query_stmt);
    if (code == SQLITE_ROW) {  // FIXME: retry on error etc
      rowid = sqlite3_column_int(query_stmt, 0);
    } else if (code == SQLITE_DONE) {
      char* zSQL = sqlite3_mprintf(
          "insert into filenames (name, basename) values (%Q, %Q)",
          file.c_str(), "todo");
      bool success = db_.exec(zSQL);
      sqlite3_free(zSQL);

      if (success)  // FIXME: retry on error etc
        rowid = sqlite3_last_insert_rowid(db_.db());
      else
        fprintf(stderr, "insert error\n");
    } else {
      fprintf(stderr, "query error %d\n", code);
    }

    sqlite3_finalize(query_stmt);

    lastFile_ = file;
    lastFileId_ = rowid;
    return rowid;
  }

  // TODO: maybe function arity,
  // maybe one of class/enum/function/struct/union, maybe file,
  // maybe isDefinition,
  // v2: referencing places (requires "linking")
  void putSymbol(int fileId, int lineNr, const std::string& symbol, char kind) {

    char* zSQL = sqlite3_mprintf(
        "insert or replace into symbols (fileid, linenr, symbol, kind) "
        "                        values (%d, %d, %Q, '%c')",
        fileId, lineNr, symbol.c_str(), kind);
    db_.exec(zSQL);
    sqlite3_free(zSQL);
  }

private:
  StupidDatabase db_;
  std::string lastFile_;
  int lastFileId_;
};


class CompletePlugin : public ASTConsumer {
public:
  CompletePlugin(CompilerInstance& instance)
      : instance_(instance),
        d_(instance.getDiagnostics()) {
    // FIXME: Emit diag instead?
    if (!db_.open(kDbPath))
      fprintf(stderr, "Failed to open db\n");
  }

  virtual void HandleTagDeclDefinition(TagDecl *D);
  virtual void HandleTopLevelDecl(DeclGroupRef D);
  virtual void HandleTranslationUnit(ASTContext &Ctx) {
    db_.flush();
  }

  void HandleDecl(Decl* decl);

private:
  CompilerInstance& instance_;
  Diagnostic& d_;
  CompletePluginDB db_;
};

static bool shouldIgnoreDecl(Decl* decl, CompilerInstance& instance) {
  SourceLocation loc = decl->getLocStart();
  SourceManager& source_manager = instance.getSourceManager();
  loc = source_manager.getInstantiationLoc(loc);

  // Ignore built-ins.
  if (loc.isInvalid()) return true;

  // Ignore stuff from system headers.
  if (source_manager.isInSystemHeader(loc)) return true;

  // Ignore everything not in the main file.
  //if (!source_manager.isFromMainFile(loc)) return true;

  // Doesn't actually save run time.
  //if (CXXRecordDecl* record = dyn_cast<CXXRecordDecl>(decl))
    //if (record->getTemplateSpecializationKind() == TSK_ImplicitInstantiation)
      //return true;

  return false;
}

void CompletePlugin::HandleTagDeclDefinition(TagDecl *D) {
return;
  if (shouldIgnoreDecl(D, instance_)) return;
  HandleDecl(D);

  for (DeclContext::decl_iterator DI = D->decls_begin(),
      DIEnd = D->decls_end();
      DI != DIEnd; ++DI) {
    if (isa<FunctionDecl>(*DI)) {
      HandleDecl(*DI);
    }
  }
}

void CompletePlugin::HandleTopLevelDecl(DeclGroupRef D) {
  for (DeclGroupRef::iterator I = D.begin(), E = D.end(); I != E; ++I) {
    if (shouldIgnoreDecl(*I, instance_)) return;
    HandleDecl(*I);
  }
}

void CompletePlugin::HandleDecl(Decl* decl) {
  // A DeclContext is something that can contain declarations, e.g. a namespace,
  // a class, or a function. Recurse into these for their declarations.
  // TODO(thakis): Do recurse into objc class definitions.
  // TODO(thakis): Maybe do not recurse into instantiated templates?
  if (DeclContext* DC = dyn_cast<DeclContext>(decl)) {
    // Do not recurse into functions. Local variables / functions are not
    // very interesting, and this is slow to do. (Without this, running the
    // plugin on this file takes 37s and produces 17876 symbols. With this,
    // it takes 27s and produces 13589 symbols. Recursing only into
    // RecordDecls and NamespaceDecls takes 25s and produces 12101 symbols.
    // Running without the plugin takes 1.6s :-/ Inserting only TagDecls
    // takes 3.4s and produces 1047 symbols. Inserting only TagDecls and their
    // child FunctionDecls by walking all TagDecl children takes 15s and
    // produces 9714 symbols)
    //
    // For the "Only TagDecls and their children case":
    // * Normal: 15s
    // * Creating tables only at program start: .5s faster
    // * FileIdCache in getFileId: .3s faster :-/
    // * Cache only last file in getFileId: .5s faster
    // * Tell sqlite to be fast: 12s faster
    //   (only .3s slower than without plugin)
    // * Use fast sqlite, collect every symbol except locals: 2s instead if 1.6s
    // * Use fast sqlite, collect every symbol: 2.2s instead if 1.6s
    if (!isa<FunctionDecl>(DC)) {
      for (DeclContext::decl_iterator DI = DC->decls_begin(),
                                   DIEnd = DC->decls_end();
           DI != DIEnd; ++DI) {
        HandleDecl(*DI);
      }
    }
  }

  if (TagDecl* td = dyn_cast<TagDecl>(decl))
    if (!td->isDefinition())
      return;  // Declarations are boring.

  if (isa<UsingShadowDecl>(decl))
    return;

  SourceLocation loc = decl->getLocStart();
  SourceManager& source_manager = instance_.getSourceManager();
  loc = source_manager.getInstantiationLoc(loc);

  if (NamedDecl* named = dyn_cast<NamedDecl>(decl)) {
    // TODO: filter out using directives.
    std::string identifier = named->getNameAsString();
    std::string filename = source_manager.getBufferName(loc);
    int fileId = db_.getFileId(filename);
    int lineNr = source_manager.getInstantiationLineNumber(loc);

    char kind = ' ';
    if (FunctionDecl* fd = dyn_cast<FunctionDecl>(named)) {
      if (fd->isThisDeclarationADefinition())
        kind = 'f';
      else
        kind = 'p';
    }
    else if (isa<CXXRecordDecl>(named)) kind = 'c';
    else if (isa<RecordDecl>(named)) kind = 's';
    else if (isa<FieldDecl>(named)) kind = 'm';
    // Field of anonymous unions
    else if (isa<IndirectFieldDecl>(named)) kind = 'm';
    else if (isa<EnumDecl>(named)) kind = 'g';
    else if (isa<EnumConstantDecl>(named)) kind = 'e';
    else if (isa<VarDecl>(named)) kind = 'v';
    else if (isa<TypedefDecl>(named)) kind = 't';
    else if (isa<TagDecl>(named)) kind = 'u';
    else if (isa<FunctionTemplateDecl>(named)) kind = 'f';  // FIXME: 'p'?
    else if (isa<ClassTemplateDecl>(named)) kind = 'c';

    // Nonstandard
    else if (isa<NamespaceDecl>(named)) kind = 'n';
    else if (isa<UsingDecl>(named)) kind = 'x';
    else if (isa<UsingDirectiveDecl>(named)) kind = 'y';
    else if (isa<NamespaceAliasDecl>(named)) kind = 'y';

    db_.putSymbol(fileId, lineNr, identifier, kind);
  }
}


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
