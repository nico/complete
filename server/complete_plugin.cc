/*
A clang plugin that works like ctags.

Since it uses clang, this plugin supports ObjC (ctags does not), and it produces
better results in C++ as well.

Build like this:
export LLVM_ROOT=$HOME/src/llvm-rw
g++ -c complete_plugin.cc \
    `$LLVM_ROOT/Release+Asserts/bin/llvm-config --cxxflags` \
    -I$LLVM_ROOT/tools/clang/include

g++ -dynamiclib -Wl,-undefined,dynamic_lookup \
    -lsqlite3 complete_plugin.o -o libcomplete_plugin.dylib

Run it like this:
time $LLVM_ROOT/Release+Asserts/bin/clang++ -c complete_plugin.cc \
    `$LLVM_ROOT/Release+Asserts/bin/llvm-config --cxxflags` \
    -I$LLVM_ROOT/tools/clang/include \
    -Xclang -load -Xclang libcomplete_plugin.dylib \
    -Xclang -add-plugin -Xclang complete \
    -Xclang -plugin-arg-complete -Xclang --db=~/builddb.sqlite

There's an optional source-root parameter;
    -Xclang -plugin-arg-complete -Xclang --source-root=~/my/project
It defaults to cwd if omitted.

Run it on the clang code (~10% slowdown compared to building without plugin):
time CXXFLAGS='-Xclang -load -Xclang /Users/thakis/src/complete/server/libcomplete_plugin.dylib -Xclang -add-plugin -Xclang complete -Xclang -plugin-arg-complete -Xclang --db=`pwd`/builddb.sqlite' \
    make  CXX=/Users/thakis/src/llvm-rw/Release+Asserts/bin/clang++ -j4

The database is meant to be converted into the ctags format later on, which
editors like vim understand. tags.py in this folder does the conversion:
  ./tags.py builddb.sqlite > tags
The tags file format description is here: http://ctags.sourceforge.net/FORMAT
*/
#include <string>

#include <wordexp.h>
#define __STDC_LIMIT_MACROS
#define __STDC_CONSTANT_MACROS 
#include "clang/Frontend/FrontendPluginRegistry.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/AST.h"
#include "clang/AST/TypeLoc.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/CompilerInstance.h"
using namespace clang;

#include "sqlite3.h"


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

      // Putting everything in one transaction makes things much faster. Less
      // robust journalling helps as well. With all these settings, run time
      // goes from 14.4s to 1.9s on this file (compared to 1.6s when running
      // without the plugin).
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

  int getFileId(const std::string& file, const std::string& source_root) {
    // TODO: should check getcwd() as well; same filename with a different
    //       |file| is not a cache hit.
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
    std::string effective_path = abspath;
    if (effective_path.find(source_root) == 0)
      effective_path = effective_path.substr(source_root.size());

    const char query[] = "select rowid from filenames where name=?";
    sqlite3_stmt* query_stmt = NULL;
    if (sqlite3_prepare_v2(
          db_, query, -1, &query_stmt, NULL) != SQLITE_OK) {
      return -1;
    }
    sqlite3_bind_text(
        query_stmt, 1, effective_path.c_str(), -1, SQLITE_TRANSIENT);

    int rowid = -1;
    int code = sqlite3_step(query_stmt);
    if (code == SQLITE_ROW) {  // FIXME: retry on error etc
      rowid = sqlite3_column_int(query_stmt, 0);
    } else if (code == SQLITE_DONE) {
      char* zSQL = sqlite3_mprintf(
          "insert into filenames (name, basename) values (%Q, %Q)",
          effective_path.c_str(), "todo");
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
    exec( "create index if not exists filename_name_idx on filenames(name)");
    exec("create index if not exists filename_basename_idx "
         "on filenames(basename)");

    exec("create table if not exists symbols "
         "    (fileid integer, linenr integer, symbol, kind, "
         "     primary key(fileid, linenr, symbol))");
  }

  sqlite3* db_;
  std::string lastFile_;
  int lastFileId_;
};


static void diagError(const CompilerInstance& CI, const std::string& err) {
	DiagnosticsEngine &D = CI.getDiagnostics();
  D.Report(D.getCustomDiagID(DiagnosticsEngine::Error, err));
}


class CompletePlugin : public ASTConsumer {
public:
  CompletePlugin(CompilerInstance& instance, const std::string& db_path,
                 const std::string& source_root)
      : instance_(instance),
        d_(instance.getDiagnostics()),
        db_path_(db_path),
        source_root_(source_root) {
  }

  virtual bool HandleTopLevelDecl(DeclGroupRef D);
  virtual void HandleTranslationUnit(ASTContext &Ctx);

  void HandleDecl(Decl* decl, CompletePluginDB& db);

private:
  CompilerInstance& instance_;
  DiagnosticsEngine& d_;
  std::string db_path_;
  std::string source_root_;
  std::vector<DeclGroupRef> declGroups_;
};

static bool shouldIgnoreDecl(Decl* decl, CompilerInstance& instance) {
  SourceLocation loc = decl->getLocStart();
  SourceManager& source_manager = instance.getSourceManager();
  loc = source_manager.getExpansionLoc(loc);

  // Ignore built-ins.
  if (loc.isInvalid()) return true;

  // Ignore stuff from system headers.
  if (source_manager.isInSystemHeader(loc)) return true;

  return false;
}

bool CompletePlugin::HandleTopLevelDecl(DeclGroupRef D) {
  declGroups_.push_back(D);
  return true; //Not sure if this is right thing to do.
}

void CompletePlugin::HandleTranslationUnit(ASTContext &Ctx) {
  CompletePluginDB db_;
  if (!db_.open(db_path_))
    diagError(instance_, "Failed to open db \"" + db_path_ + "\"");
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
    if (!td->isCompleteDefinition())
      return;  // Declarations are boring.

  if (isa<UsingShadowDecl>(decl) || isa<UsingDirectiveDecl>(decl))
    return;

  SourceLocation loc = decl->getLocStart();
  SourceManager& source_manager = instance_.getSourceManager();
  loc = source_manager.getExpansionLoc(loc);

  if (NamedDecl* named = dyn_cast<NamedDecl>(decl)) {
    std::string identifier = named->getNameAsString();
    std::string filename = source_manager.getBufferName(loc);
    int fileId = db_.getFileId(filename, source_root_);
    int lineNr = source_manager.getExpansionLineNumber(loc);

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


static std::string expand(std::string s) {
  // Expand ~
  wordexp_t p;
  if (wordexp(s.c_str(), &p, 0) == 0 && p.we_wordc > 0)
    s = p.we_wordv[0];
  wordfree(&p);
  return s;
}


class CompletePluginAction : public PluginASTAction {
 protected:
  ASTConsumer* CreateASTConsumer(CompilerInstance &CI, llvm::StringRef ref) {
    return new CompletePlugin(CI, db_path_, source_root_);
  }

  bool ParseArgs(const CompilerInstance &CI,
                 const std::vector<std::string>& args) {
    for (unsigned i = 0, e = args.size(); i != e; ++i) {
      // Manual flag parsing. Boo!
      const std::string& arg = args[i];

      std::string db_flag("--db=");
      if (!arg.compare(0, db_flag.size(), db_flag))
        db_path_ = expand(arg.substr(db_flag.size()));

      std::string source_root_flag("--source-root=");
      if (!arg.compare(0, source_root_flag.size(), source_root_flag))
        source_root_ = expand(arg.substr(source_root_flag.size()));
    }

    if (db_path_ == "")
      diagError(CI, "complete plugin expects '-plugin-arg-complete --db=path'");

    if (source_root_ == "") {
      // TODO(thakis): Does relative to db_path_ make more sense?
      char* cwd = getcwd(NULL, 0);
      source_root_ = cwd;
      source_root_ += "/";
      free(cwd);
    }

    return true;
  }
 private:
  std::string db_path_;
  std::string source_root_;
};


static FrontendPluginRegistry::Add<CompletePluginAction>
X("complete", "Adds information about symbols to a sqlite database.");
