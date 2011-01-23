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
    const char query[] = "select rowid from filenames where name=?"; // % name)
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
      const char insert[] =
          "insert into filenames (name, basename) values (?, ?)"; // % name)
      sqlite3_stmt* insert_stmt = NULL;
      if (sqlite3_prepare_v2(
            db_.db(), insert, -1, &insert_stmt, NULL) != SQLITE_OK) {
        // FIXME: leaks
        return -1;
      }
      sqlite3_bind_text(insert_stmt, 1, file.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(insert_stmt, 2, "todo", -1, SQLITE_TRANSIENT);

      int code = sqlite3_step(insert_stmt);
      if (code == SQLITE_DONE) {  // FIXME: retry on error etc
        rowid = sqlite3_last_insert_rowid(db_.db());
      } else {
        fprintf(stderr, "insert error %d\n", code);
      }
      sqlite3_finalize(insert_stmt);
    } else {
      fprintf(stderr, "query error %d\n", code);
    }

    sqlite3_finalize(query_stmt);

    return rowid;
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

private:
  CompilerInstance& instance_;
  Diagnostic& d_;
  CompletePluginDB db_;
};

void CompletePlugin::HandleTopLevelDecl(DeclGroupRef D) {
  for (DeclGroupRef::iterator I = D.begin(), E = D.end(); I != E; ++I) {
    if (NamedDecl* record = dyn_cast<NamedDecl>(*I)) {
      SourceLocation record_location = record->getLocStart();
      SourceManager& source_manager = instance_.getSourceManager();

      record_location = source_manager.getInstantiationLoc(record_location);

      // Ignore built-ins.
      if (record_location.isInvalid()) return;

      // Ignore stuff from system headers.
      if (source_manager.isInSystemHeader(record_location)) return;

      // Ignore everything not in the main file.
      //if (!source_manager.isFromMainFile(record_location)) return;

      //std::string identifier = record->getIdentifier();
      std::string identifier = record->getNameAsString();
      std::string filename = source_manager.getBufferName(record_location);
      int fileId = db_.getFileId(filename);
      fprintf(stderr, "%d %s:%u Identifier: %s\n",
          fileId,
          filename.c_str(),
          source_manager.getInstantiationLineNumber(record_location),
          identifier.c_str());
    }
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
