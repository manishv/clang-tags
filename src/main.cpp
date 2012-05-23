#include <clang/AST/ASTConsumer.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendAction.h>
#include <clang/Frontend/FrontendActions.h>
#include <clang/Tooling/CompilationDatabase.h>
#include <clang/Tooling/Tooling.h>

#include <stdexcept>
#include <sstream>
#include <iostream>
#include <vector>

#include <sqlite3.h>
#include <sys/unistd.h>

using namespace llvm;
using namespace clang;
using namespace tooling;

class TagsDeclInfo
{
public:
  int id;
  std::string filename;
  std::string text;
  int line_no;
  int col_no;
  std::string name;
};

class TagsDatabase
{
public:
  virtual void add_declaration(ASTContext *Context, NamedDecl *Declaration) = 0;

  virtual std::vector<TagsDeclInfo>
  find_declaration(const std::string& name) = 0;
};

inline void sql_chk(int return_code) {
  if (return_code != SQLITE_OK)
    throw std::runtime_error("SQLite3 call failed");
}

const char * tags_sql = "\
CREATE TABLE SourcePaths (                                              \
    id INTEGER PRIMARY KEY,                                             \
                                                                        \
    dirname_id INTEGER,                                                 \
    pathname VARCHAR(4096) NOT NULL,                                    \
                                                                        \
    FOREIGN KEY(dirname_id) REFERENCES SourcePaths(id)                  \
);                                                                      \
CREATE UNIQUE INDEX SourcePaths_id_idx                                  \
    ON SourcePaths (id);                                                \
CREATE INDEX SourcePaths_pathname_idx                                   \
    ON SourcePaths (pathname);                                          \
CREATE UNIQUE INDEX SourcePaths_all_idx                                 \
    ON SourcePaths (dirname_id, pathname);                              \
                                                                        \
CREATE TABLE SourceLines (                                              \
    id INTEGER PRIMARY KEY,                                             \
                                                                        \
    source_path_id INTEGER NOT NULL,                                    \
    lineno         INTEGER NOT NULL,                                    \
    text           TEXT NOT NULL,                                       \
                                                                        \
    FOREIGN KEY(source_path_id) REFERENCES SourcePaths(id)              \
);                                                                      \
CREATE UNIQUE INDEX SourceLines_id_idx                                  \
    ON SourceLines (id);                                                \
                                                                        \
CREATE TABLE DeclKinds (                                                \
    id INTEGER PRIMARY KEY,                                             \
                                                                        \
    description CHAR(40) NOT NULL                                       \
);                                                                      \
                                                                        \
INSERT INTO DeclKinds (description) VALUES (\"function\");              \
INSERT INTO DeclKinds (description) VALUES (\"type\");                  \
INSERT INTO DeclKinds (description) VALUES (\"variable\");              \
INSERT INTO DeclKinds (description) VALUES (\"enum\");                  \
INSERT INTO DeclKinds (description) VALUES (\"macro\");                 \
INSERT INTO DeclKinds (description) VALUES (\"namespace\");             \
                                                                        \
CREATE TABLE SymbolNames (                                              \
    id INTEGER PRIMARY KEY,                                             \
                                                                        \
    short_name TEXT NOT NULL,                                           \
    full_name  TEXT NOT NULL                                            \
);                                                                      \
CREATE UNIQUE INDEX SymbolNames_id_idx                                  \
    ON SymbolNames (id);                                                \
CREATE INDEX SymbolNames_short_name_idx                                 \
    ON SymbolNames (short_name);                                        \
CREATE UNIQUE INDEX SymbolNames_full_name_idx                           \
    ON SymbolNames (full_name);                                         \
CREATE UNIQUE INDEX SymbolNames_all_idx                                 \
    ON SymbolNames (short_name, full_name);                             \
                                                                        \
CREATE TABLE Declarations (                                             \
    id INTEGER PRIMARY KEY,                                             \
                                                                        \
    symbol_name_id TEXT NOT NULL,                                       \
    kind_id        INTEGER NOT NULL,                                    \
    is_definition  INTEGER NOT NULL,                                    \
                                                                        \
    FOREIGN KEY(symbol_name_id) REFERENCES SymbolNames(id),             \
    FOREIGN KEY(kind_id)        REFERENCES DeclKinds(id)                \
);                                                                      \
CREATE UNIQUE INDEX Declarations_id_idx                                 \
    ON Declarations (id);                                               \
CREATE INDEX Declarations_symbol_name_id_idx                            \
    ON Declarations (symbol_name_id);                                   \
CREATE INDEX Declarations_kind_id_idx                                   \
    ON Declarations (kind_id);                                          \
CREATE INDEX Declarations_is_definition_idx                             \
    ON Declarations (is_definition);                                    \
CREATE INDEX Declarations_all_idx                                       \
    ON Declarations (symbol_name_id, kind_id, is_definition);           \
                                                                        \
CREATE TABLE DeclRefKinds (                                             \
    id INTEGER PRIMARY KEY,                                             \
                                                                        \
    description CHAR(40) NOT NULL                                       \
);                                                                      \
                                                                        \
INSERT INTO DeclRefKinds (description) VALUES (\"definition\");         \
INSERT INTO DeclRefKinds (description) VALUES (\"declaration\");        \
INSERT INTO DeclRefKinds (description) VALUES (\"use\");                \
                                                                        \
CREATE TABLE DeclRefs (                                                 \
       id INTEGER PRIMARY KEY,                                          \
                                                                        \
       declaration_id INTEGER NOT NULL,                                 \
       ref_kind_id    INTEGER NOT NULL,                                 \
       source_line_id INTEGER NOT NULL,                                 \
       colno          INTEGER NOT NULL,                                 \
       is_implicit    INTEGER NOT NULL,                                 \
                                                                        \
       context_ref_id INTEGER,                                          \
                                                                        \
       FOREIGN KEY(declaration_id) REFERENCES Entities(id),             \
       FOREIGN KEY(ref_kind_id)    REFERENCES DeclRefKinds(id),         \
       FOREIGN KEY(source_line_id) REFERENCES SourceLines(id)           \
       FOREIGN KEY(context_ref_id) REFERENCES DeclRefs(id)              \
);                                                                      \
CREATE UNIQUE INDEX DeclRefs_id_idx                                     \
    ON DeclRefs (id);                                                   \
CREATE INDEX DeclRefs_declaration_id_idx                                \
    ON DeclRefs (declaration_id);                                       \
CREATE INDEX DeclRefs_ref_kind_id_idx                                   \
    ON DeclRefs (ref_kind_id);                                          \
CREATE INDEX DeclRefs_line_col_idx                                      \
    ON DeclRefs (source_line_id, colno);                                \
CREATE INDEX DeclRefs_is_implicit_idx                                   \
    ON DeclRefs (is_implicit);                                          \
                                                                        \
CREATE TABLE SchemaInfo (                                               \
       version INTEGER                                                  \
);                                                                      \
                                                                        \
INSERT INTO SchemaInfo (version) VALUES (\"1\");";

int query_callback(void *a_param, int argc, char **argv, char **column)
{
  assert(argc == 1);
  *reinterpret_cast<long *>(a_param) = std::atoi(argv[0]);
  return 0;
}

class SqliteTagsDatabase : public TagsDatabase
{
  sqlite3 *database;

public:
  SqliteTagsDatabase(const std::string& path) {
    sql_chk(sqlite3_initialize());

    bool exists = false;
    if (access(path.c_str(), R_OK) == 0)
      exists = true;

    try {
      sql_chk(sqlite3_open(path.c_str(), &database));

      try {
        if (! exists) {
          char * error_msg;
          try {
            sql_chk(sqlite3_exec(
                      database, tags_sql, NULL, NULL, &error_msg));
          }
          catch (const std::exception& err) {
            std::cerr << "SQLite3 error: " << error_msg << std::endl;
            std::cerr << "Error occurred with the following statement: "
                      << std::endl << tags_sql << std::endl;
            throw;
          }
        }
      }
      catch (...) {
        sqlite3_close(database);
      }
    }
    catch (...) {
      sqlite3_shutdown();
    }
  }

  virtual ~SqliteTagsDatabase() {
    sql_chk(sqlite3_close(database));
    sql_chk(sqlite3_shutdown());
  }

  long sqlite3_query_for_id(const char * sql)
  {
    char * error_msg;
    try {
      long id = -1;
      sql_chk(sqlite3_exec(database, sql, query_callback, &id, &error_msg));
      return id;
    }
    catch (const std::exception& err) {
      std::cerr << "SQLite3 error: " << error_msg << std::endl;
      std::cerr << "Error occurred with the following statement: "
                << std::endl << sql << std::endl;
      throw;
    }
  }

  long sqlite3_insert_new(const char * sql)
  {
    char * error_msg;
    try {
      sql_chk(sqlite3_exec(database, sql, NULL, NULL, &error_msg));
      return static_cast<long>(sqlite3_last_insert_rowid(database));
    }
    catch (const std::exception& err) {
      std::cerr << "SQLite3 error: " << error_msg << std::endl;
      std::cerr << "Error occurred with the following statement: "
                << std::endl << sql << std::endl;
      throw;
    }
  }

  long sqlite3_insert_maybe(
    const char * select_sql, const char * insert_sql, ...)
  {
    va_list argslist;
    va_start(argslist, insert_sql);

    long id = sqlite3_query_for_id(sqlite3_vmprintf(select_sql, argslist));
    if (id == -1) {
      va_start(argslist, insert_sql);
      id = sqlite3_insert_new(sqlite3_vmprintf(insert_sql, argslist));
    }
    return id;
  }

  virtual void add_declaration(ASTContext *Context, NamedDecl *Declaration)
  {
    if (Declaration->getNameAsString().empty())
      return;

    FullSourceLoc FullLocation =
      Context->getFullLoc(Declaration->getLocStart());

    FileID file_id = FullLocation.getFileID();
    const FileEntry * file_entry =
      FullLocation.getManager().getFileEntryForID(file_id);
    if (! file_entry)
      return;

    std::pair<FileID, unsigned> LocInfo = FullLocation.getDecomposedLoc();

    const llvm::MemoryBuffer *Buffer      = FullLocation.getBuffer();
    const char *              BufferStart = Buffer->getBufferStart();
    std::size_t offset                    =
      LocInfo.second - (FullLocation.getSpellingColumnNumber() - 1);

    const char * LineBegin = BufferStart + offset;
    const char * LineEnd = LineBegin;
    while (*LineEnd && *LineEnd != '\n')
      ++LineEnd;

    std::string LineBuf(LineBegin, LineEnd);

    long source_path_dirname_id =
      sqlite3_insert_maybe(
        "SELECT id FROM SourcePaths WHERE pathname = '%q'",
        "INSERT INTO SourcePaths (pathname) VALUES ('%q');",
        file_entry->getDir()->getName());

    long source_path_id =
      sqlite3_insert_maybe(
        "SELECT id FROM SourcePaths WHERE dirname_id = %d AND pathname = '%q'",
        "INSERT INTO SourcePaths (dirname_id, pathname) VALUES (%d, '%q');",
        source_path_dirname_id, file_entry->getName());

    long source_line_id =
      sqlite3_insert_maybe(
        "SELECT id FROM SourceLines \
             WHERE source_path_id = %d AND lineno = %d AND text = '%q'",
        "INSERT INTO SourceLines (source_path_id, lineno, text) \
             VALUES (%d, %d, '%q');",
        source_path_id, FullLocation.getSpellingLineNumber(), LineBuf.c_str());

    long symbol_name_id =
      sqlite3_insert_maybe(
        "SELECT id FROM SymbolNames \
             WHERE short_name = '%q' AND full_name = '%q'",
        "INSERT INTO SymbolNames (short_name, full_name) \
             VALUES ('%q', '%q');",
        Declaration->getNameAsString().c_str(),
        Declaration->getQualifiedNameAsString().c_str());

    long declaration_id =
      sqlite3_insert_maybe(
        "SELECT id FROM Declarations \
             WHERE symbol_name_id = %d AND kind_id = %d AND is_definition = %d",
        "INSERT INTO Declarations (symbol_name_id, kind_id, is_definition) \
             VALUES (%d, %d, %d);",
        symbol_name_id, 1, 1);

#if 0
    long decl_ref_id =
#endif
      sqlite3_insert_maybe(
        "SELECT id FROM DeclRefs \
             WHERE declaration_id = %d AND ref_kind_id = %d AND \
                   source_line_id = %d AND colno = %d AND is_implicit = %d",
        "INSERT INTO DeclRefs (declaration_id, ref_kind_id, source_line_id, \
                               colno, is_implicit) \
             VALUES (%d, %d, %d, %d, %d);",
        declaration_id, 1, source_line_id, FullLocation.getSpellingColumnNumber(), 0);
  }

  virtual std::vector<TagsDeclInfo> find_declaration(const std::string& name)
  {
    char * sql = sqlite3_mprintf(
      "\
SELECT                                                  \
    Declarations.id,                                    \
    SourcePaths.dirname,                                \
    SourcePaths.pathname,                               \
    SourceLines.lineno,                                 \
    DeclRefs.colno,                                     \
    SourceLines.text                                    \
FROM                                                    \
    DeclRefs,                                           \
    Declarations,                                       \
    SourcePaths,                                        \
    SourceLines                                         \
WHERE                                                   \
    DeclRefs.declaration_id     = Declarations.id       \
AND DeclRefs.source_line_id     = SourceLines.id        \
AND SourceLines.source_path_id  = SourcePaths.id        \
AND Declarations.symbol_name_id =                       \
    (                                                   \
        SELECT                                          \
            id                                          \
        FROM                                            \
            SymbolNames                                 \
        WHERE                                           \
            full_name = '%q');", name.c_str());

    try {
      sqlite3_stmt *stmt;
      sql_chk(sqlite3_prepare(database, sql, std::strlen(sql), &stmt, NULL));

      std::vector<TagsDeclInfo> tags;
      while (sqlite3_step(stmt) == SQLITE_ROW) {
        TagsDeclInfo info;
        info.name     = name;
        info.id       = sqlite3_column_int(stmt, 0);
        info.filename =
          (std::string(
            reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1)))
           + "/" + reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2)));
        info.line_no  = sqlite3_column_int(stmt, 3);
        info.col_no   = sqlite3_column_int(stmt, 4);
        info.text     = (reinterpret_cast<const char *>(
                           sqlite3_column_text(stmt, 5)));
        tags.push_back(info);
      }

      sql_chk(sqlite3_finalize(stmt));
      return tags;
    }
    catch (const std::exception& err) {
      std::cerr << "Error occurred with the following query: "
                << std::endl << sql << std::endl;
      throw;
    }
  }
};

class TagsClassVisitor : public RecursiveASTVisitor<TagsClassVisitor>
{
  TagsDatabase& tags_db;

public:
  explicit TagsClassVisitor(ASTContext *Context, TagsDatabase& db)
    : tags_db(db), Context(Context) {}
  virtual ~TagsClassVisitor() {}

  bool VisitNamedDecl(NamedDecl *Declaration) {
    tags_db.add_declaration(Context, Declaration);
    return true;
  }

private:
  ASTContext *Context;
};

class TagsClassConsumer : public ASTConsumer
{
public:
  explicit TagsClassConsumer(ASTContext *Context, TagsDatabase& db)
    : Visitor(Context, db) {}
  virtual ~TagsClassConsumer() {}

  virtual void HandleTranslationUnit(ASTContext& Context) {
    Visitor.TraverseDecl(Context.getTranslationUnitDecl());
  }

private:
  TagsClassVisitor Visitor;
};

class TagsClassAction : public ASTFrontendAction
{
  TagsDatabase& db;

public:
  TagsClassAction(TagsDatabase& db) : db(db) {}
  virtual ~TagsClassAction() {}

  virtual ASTConsumer *CreateASTConsumer(
    CompilerInstance& Compiler, llvm::StringRef InFile) {
    return new TagsClassConsumer(&Compiler.getASTContext(), db);
  }
};

class TagsClassActionFactory : public FrontendActionFactory
{
  TagsDatabase& db;
public:
  explicit TagsClassActionFactory(TagsDatabase& db) : db(db) {}

  virtual FrontendAction *create() { return new TagsClassAction(db); }
};

cl::opt<std::string> BuildPath(
  cl::Positional,
  cl::desc("<build-path>"));

cl::list<std::string> SourcePaths(
  cl::Positional,
  cl::desc("<source0> [... <sourceN>]"),
  cl::OneOrMore);

int main(int argc, char **argv) {
  if (argc > 1) {
    SqliteTagsDatabase tags_db("./CLTAGS");

    if (std::string(argv[1]) == "decl") {
      std::vector<TagsDeclInfo> tags(tags_db.find_declaration(argv[2]));
      for (std::vector<TagsDeclInfo>::const_iterator i = tags.begin();
           i != tags.end();
           ++i)
        std::cout << (*i).filename << ":"
                  << (*i).line_no << ":" << (*i).col_no << ":" << (*i).text
                  << std::endl;
    } else {
      cl::ParseCommandLineOptions(argc, argv);

      std::string ErrorMessage;
      llvm::OwningPtr<CompilationDatabase> Compilations(
        CompilationDatabase::loadFromDirectory(BuildPath, ErrorMessage));
      if (!Compilations)
        llvm::report_fatal_error(ErrorMessage);

      // We hand the CompilationDatabase we created and the sources to run over into
      // the tool constructor.
      ClangTool Tool(*Compilations, SourcePaths);

      // The ClangTool needs a new FrontendAction for each translation unit we run
      // on. Thus, it takes a FrontendActionFactory as parameter. To create a
      // FrontendActionFactory from a given FrontendAction type, we call
      // newFrontendActionFactory<SyntaxOnlyAction>().
      return Tool.run(new TagsClassActionFactory(tags_db));
    }
  }
}

// main.cpp ends here
