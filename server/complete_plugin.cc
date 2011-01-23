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

  select symbol, name, linenr \
  from symbols join filenames on symbols.fileid = filenames.rowid \
  where symbol = 'begin';

*/
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
    return db_.open(file);
  }

  int getFileId(const std::string& file) {
    db_.exec("create table if not exists filenames(name, basename)");
    db_.exec(
        "create index if not exists filename_name_idx on filenames(name)");
    db_.exec(
        "create index if not exists filename_basename_idx "
        "on filenames(basename)");

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

    return rowid;
  }

  // TODO: kind, maybe function arity,
  // maybe one of class/enum/function/struct/union, maybe file,
  // maybe isDefinition,
  // v2: referencing places (requires "linking")
  void putSymbol(int fileId, int lineNr, const std::string& symbol) {
    db_.exec(
        "create table if not exists symbols "
        "    (fileid integer, linenr integer, symbol, "
        "     primary key(fileid, linenr, symbol))");

    char* zSQL = sqlite3_mprintf(
        "insert or replace into symbols (fileid, linenr, symbol) "
        "                        values (%d, %d, %Q)",
        fileId, lineNr, symbol.c_str());
    db_.exec(zSQL);
    sqlite3_free(zSQL);
  }

private:
  StupidDatabase db_;
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

  virtual void HandleTopLevelDecl(DeclGroupRef D);

  void HandleDecl(Decl* decl);

private:
  CompilerInstance& instance_;
  Diagnostic& d_;
  CompletePluginDB db_;
};

void CompletePlugin::HandleTopLevelDecl(DeclGroupRef D) {
  for (DeclGroupRef::iterator I = D.begin(), E = D.end(); I != E; ++I) {
    Decl* decl = *I;
    SourceLocation loc = decl->getLocStart();
    SourceManager& source_manager = instance_.getSourceManager();
    loc = source_manager.getInstantiationLoc(loc);

    // Ignore built-ins.
    if (loc.isInvalid()) return;

    // Ignore stuff from system headers.
    if (source_manager.isInSystemHeader(loc)) return;

    // Ignore everything not in the main file.
    //if (!source_manager.isFromMainFile(loc)) return;

    HandleDecl(decl);
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
    // Running without the plugin takes 1.6s :-/)
    if (!isa<FunctionDecl>(DC)) {
      for (DeclContext::decl_iterator DI = DC->decls_begin(),
                                   DIEnd = DC->decls_end();
           DI != DIEnd; ++DI) {
        HandleDecl(*DI);
      }
    }
  }

  SourceLocation loc = decl->getLocStart();
  SourceManager& source_manager = instance_.getSourceManager();
  loc = source_manager.getInstantiationLoc(loc);

  if (NamedDecl* named = dyn_cast<NamedDecl>(decl)) {
    // TODO: filter out using directives.
    std::string identifier = named->getNameAsString();
    std::string filename = source_manager.getBufferName(loc);
    int fileId = db_.getFileId(filename);
    int lineNr = source_manager.getInstantiationLineNumber(loc);
    db_.putSymbol(fileId, lineNr, identifier);
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
