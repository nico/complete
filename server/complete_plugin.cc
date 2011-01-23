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

time CXXFLAGS='-Xclang -load -Xclang /Users/thakis/src/complete/server/libcomplete_plugin.dylib -Xclang -pipeline-plugin -Xclang complete' \
    make  CXX=/Users/thakis/src/llvm-rw/Release+Asserts/bin/clang++ -j4

The database is meant to be converted into this format later on (see tags.py):
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
const char kDbPath[] = "/Users/thakis/builddb.sqlite";
//const char kDbPath[] = "builddb.sqlite";


class CompletePluginDB {
public:
  CompletePluginDB() : db_(NULL) {}
  ~CompletePluginDB() { close(); }

  bool open(const std::string& file) {
    assert(!db_);
    int err = sqlite3_open(file.c_str(), &db_);
    bool success = err == SQLITE_OK;
    if (success) {
      // Sleep up to 1200 seconds / 20 minutes on busy.
      sqlite3_busy_timeout(db_, 100000);

      // Putting everything in one transaction is the biggest win. With
      // all these settings, run time goes from 14.4s to 1.9s (compared to
      // 1.6s when running without the plugin).
      exec("pragma synchronous = off");  // 14.4s -> 7.6s
      exec("pragma journal_mode = memory");  // 14.4s -> 6.6s
      //exec("pragma journal_mode = off");  // about the same as "memory"

      prepareTables();

      // Everything in 1 transaction: 14.4s -> 2.6s
      // Make sure only one process accesses the db at a time.
      exec("begin exclusive transaction");
    }
    return success;
  }

  void close() {
    if (db_) {
      exec("end transaction");
      sqlite3_close(db_);
    }
    db_ = NULL;
  }

  int getFileId(const std::string& file) {
    // TODO: should check getcwd() as well.
    if (file == lastFile_) return lastFileId_;

    // Without this, headers "lib/Frontend/../../include/foo.h" and
    // "lib/Parser/../../include/foo.h" won't resolve to the same file.
    // For tag files, the filenames need to be absolute, too (or relative to
    // a single folder).
    // Since realpath() resolves symlinks, it needs to fstat(), which is slow.
    // Hence, this needs to be done after the cache check above.
    // TODO: clang already knows the complete path, get it from there somehow.
    char abspath[PATH_MAX];
    realpath(file.c_str(), abspath);

    const char query[] = "select rowid from filenames where name=?";
    sqlite3_stmt* query_stmt = NULL;
    if (sqlite3_prepare_v2(
          db_, query, -1, &query_stmt, NULL) != SQLITE_OK) {
      return -1;
    }
    sqlite3_bind_text(query_stmt, 1, abspath, -1, SQLITE_TRANSIENT);

    int rowid = -1;
    int code = sqlite3_step(query_stmt);
    if (code == SQLITE_ROW) {  // FIXME: retry on error etc
      rowid = sqlite3_column_int(query_stmt, 0);
    } else if (code == SQLITE_DONE) {
      char* zSQL = sqlite3_mprintf(
          "insert into filenames (name, basename) values (%Q, %Q)",
          abspath, "todo");
      bool success = exec(zSQL);
      sqlite3_free(zSQL);

      if (success)  // FIXME: retry on error etc
        rowid = sqlite3_last_insert_rowid(db_);
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
    exec(zSQL);
    sqlite3_free(zSQL);
  }

private:
  bool exec(const std::string& query) {
    if (!db_)
      return false;
    return sqlite3_exec(db_, query.c_str(), NULL, NULL, NULL) == SQLITE_OK;
  }

  void prepareTables() {
    exec("create table if not exists filenames(name, basename)");
    exec(
        "create index if not exists filename_name_idx on filenames(name)");
    exec(
        "create index if not exists filename_basename_idx "
        "on filenames(basename)");

    exec(
        "create table if not exists symbols "
        "    (fileid integer, linenr integer, symbol, kind, "
        "     primary key(fileid, linenr, symbol))");
  }

  sqlite3* db_;
  std::string lastFile_;
  int lastFileId_;
};


class CompletePlugin : public ASTConsumer {
public:
  CompletePlugin(CompilerInstance& instance)
      : instance_(instance),
        d_(instance.getDiagnostics()) {
  }

  virtual void HandleTopLevelDecl(DeclGroupRef D);
  virtual void HandleTranslationUnit(ASTContext &Ctx);

  void HandleDecl(Decl* decl, CompletePluginDB& db);

private:
  CompilerInstance& instance_;
  Diagnostic& d_;
  std::vector<DeclGroupRef> declGroups_;
};

static bool shouldIgnoreDecl(Decl* decl, CompilerInstance& instance) {
  SourceLocation loc = decl->getLocStart();
  SourceManager& source_manager = instance.getSourceManager();
  loc = source_manager.getInstantiationLoc(loc);

  // Ignore built-ins.
  if (loc.isInvalid()) return true;

  // Ignore stuff from system headers.
  if (source_manager.isInSystemHeader(loc)) return true;

  return false;
}

void CompletePlugin::HandleTopLevelDecl(DeclGroupRef D) {
  declGroups_.push_back(D);
}

void CompletePlugin::HandleTranslationUnit(ASTContext &Ctx) {
  CompletePluginDB db_;
  // FIXME: Emit diag instead?
  if (!db_.open(kDbPath))
    fprintf(stderr, "Failed to open db\n");
  for (std::vector<DeclGroupRef>::iterator vI = declGroups_.begin(),
                                           vE = declGroups_.end();
      vI != vE; ++vI) {
    for (DeclGroupRef::iterator I = vI->begin(), E = vI->end(); I != E; ++I) {
      if (shouldIgnoreDecl(*I, instance_)) continue;
      HandleDecl(*I, db_);
    }
  }
  db_.close();
}

void CompletePlugin::HandleDecl(Decl* decl, CompletePluginDB& db_) {
  // A DeclContext is something that can contain declarations, e.g. a namespace,
  // a class, or a function. Recurse into these for their declarations.
  if (DeclContext* DC = dyn_cast<DeclContext>(decl)) {
    // Do not recurse into functions. Local variables / functions are not
    // very interesting.
    if (!isa<FunctionDecl>(DC)) {
      for (DeclContext::decl_iterator DI = DC->decls_begin(),
                                   DIEnd = DC->decls_end();
           DI != DIEnd; ++DI) {
        HandleDecl(*DI, db_);
      }
    }
  }

  if (TagDecl* td = dyn_cast<TagDecl>(decl))
    if (!td->isDefinition())
      return;  // Declarations are boring.

  if (isa<UsingShadowDecl>(decl) || isa<UsingDirectiveDecl>(decl))
    return;

  SourceLocation loc = decl->getLocStart();
  SourceManager& source_manager = instance_.getSourceManager();
  loc = source_manager.getInstantiationLoc(loc);

  if (NamedDecl* named = dyn_cast<NamedDecl>(decl)) {
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
    else if (isa<FunctionTemplateDecl>(named)) {
      kind = 'f';  // FIXME: 'p'?
    }
    else if (isa<ClassTemplateDecl>(named)) kind = 'c';

    // ObjC
    // ObjCContainerDecl is the superclass for objc interfaces, implementations,
    // categories, and protocols. Use 'c' for everything.
    else if (isa<ObjCContainerDecl>(named)) kind = 'c';
    else if (isa<ObjCIvarDecl>(named)) kind = 'v';
    else if (ObjCMethodDecl* om = dyn_cast<ObjCMethodDecl>(named)) {
      // This handles property declarations as well.
      if (om->isThisDeclarationADefinition())
        kind = 'f';
      else
        kind = 'p';
    }
    // TODO: ObjCPropertyImplDecl (@synthesize) support would be nice.

    // Nonstandard tag kinds for namespaces.
    else if (isa<NamespaceDecl>(named)) kind = 'n';
    else if (isa<UsingDecl>(named)) kind = 'x';
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
