#include <clang/AST/ASTConsumer.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendAction.h>
#include <clang/Tooling/Tooling.h>

#include <stdexcept>
#include <sstream>
#include <iostream>
#include <vector>

#include <sqlite3.h>
#include <sys/unistd.h>

using namespace clang;

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
  virtual void add_declaration(
    ASTContext *Context, CXXRecordDecl *Declaration) = 0;

  virtual std::vector<TagsDeclInfo>
  find_declaration(const std::string& name) = 0;
};

inline void sql_chk(int return_code) {
  if (return_code != SQLITE_OK)
    throw std::runtime_error("SQLite3 call failed");
}

const char * tags_sql = "                                        \
CREATE TABLE SourcePaths (                                       \
    id INTEGER PRIMARY KEY,                                      \
                                                                 \
    dirname VARCHAR(4096) NOT NULL,                              \
    pathname VARCHAR(256) NOT NULL                               \
);                                                               \
CREATE UNIQUE INDEX SourcePaths_id_idx                           \
    ON SourcePaths (id);                                         \
CREATE INDEX SourcePaths_pathname_idx                            \
    ON SourcePaths (pathname);                                   \
                                                                 \
CREATE TABLE SourceLines (                                       \
    id INTEGER PRIMARY KEY,                                      \
                                                                 \
    source_path_id INTEGER NOT NULL,                             \
    lineno         INTEGER NOT NULL,                             \
    text           TEXT NOT NULL,                                \
                                                                 \
    FOREIGN KEY(source_path_id) REFERENCES SourcePaths(id)       \
);                                                               \
CREATE UNIQUE INDEX SourceLines_id_idx                           \
    ON SourceLines (id);                                         \
                                                                 \
CREATE TABLE DeclKinds (                                         \
    id INTEGER PRIMARY KEY,                                      \
                                                                 \
    description CHAR(40) NOT NULL                                \
);                                                               \
                                                                 \
INSERT INTO DeclKinds (description) VALUES (\"function\");       \
INSERT INTO DeclKinds (description) VALUES (\"type\");           \
INSERT INTO DeclKinds (description) VALUES (\"variable\");       \
INSERT INTO DeclKinds (description) VALUES (\"enum\");           \
INSERT INTO DeclKinds (description) VALUES (\"macro\");          \
INSERT INTO DeclKinds (description) VALUES (\"namespace\");      \
                                                                 \
CREATE TABLE SymbolNames (                                       \
    id INTEGER PRIMARY KEY,                                      \
                                                                 \
    short_name TEXT NOT NULL,                                    \
    full_name  TEXT NOT NULL                                     \
);                                                               \
CREATE UNIQUE INDEX SymbolNames_id_idx                           \
    ON SymbolNames (id);                                         \
CREATE UNIQUE INDEX SymbolNames_short_name_idx                   \
    ON SymbolNames (short_name);                                 \
CREATE UNIQUE INDEX SymbolNames_full_name_idx                    \
    ON SymbolNames (full_name);                                  \
                                                                 \
CREATE TABLE Declarations (                                      \
    id INTEGER PRIMARY KEY,                                      \
                                                                 \
    symbol_name_id TEXT NOT NULL,                                \
    kind_id        INTEGER NOT NULL,                             \
    is_definition  INTEGER NOT NULL,                             \
                                                                 \
    FOREIGN KEY(symbol_name_id) REFERENCES SymbolNames(id),      \
    FOREIGN KEY(kind_id)        REFERENCES DeclKinds(id)         \
);                                                               \
CREATE UNIQUE INDEX Declarations_id_idx                          \
    ON Declarations (id);                                        \
CREATE UNIQUE INDEX Declarations_symbol_name_id_idx              \
    ON Declarations (symbol_name_id);                            \
CREATE INDEX Declarations_kind_id_idx                            \
    ON Declarations (kind_id);                                   \
CREATE INDEX Declarations_is_definition_idx                      \
    ON Declarations (is_definition);                             \
                                                                 \
CREATE TABLE DeclRefKinds (                                      \
    id INTEGER PRIMARY KEY,                                      \
                                                                 \
    description CHAR(40) NOT NULL                                \
);                                                               \
                                                                 \
INSERT INTO DeclRefKinds (description) VALUES (\"definition\");  \
INSERT INTO DeclRefKinds (description) VALUES (\"declaration\"); \
INSERT INTO DeclRefKinds (description) VALUES (\"use\");         \
                                                                 \
CREATE TABLE DeclRefs (                                          \
       id INTEGER PRIMARY KEY,                                   \
                                                                 \
       declaration_id INTEGER NOT NULL,                          \
       ref_kind_id    INTEGER NOT NULL,                          \
       source_line_id INTEGER NOT NULL,                          \
       colno          INTEGER NOT NULL,                          \
       is_implicit    INTEGER NOT NULL,                          \
                                                                 \
       context_ref_id INTEGER,                                   \
                                                                 \
       FOREIGN KEY(declaration_id) REFERENCES Entities(id),      \
       FOREIGN KEY(ref_kind_id)    REFERENCES DeclRefKinds(id),  \
       FOREIGN KEY(source_line_id) REFERENCES SourceLines(id)    \
       FOREIGN KEY(context_ref_id) REFERENCES DeclRefs(id)       \
);                                                               \
CREATE UNIQUE INDEX DeclRefs_id_idx                              \
    ON DeclRefs (id);                                            \
CREATE INDEX DeclRefs_declaration_id_idx                         \
    ON DeclRefs (declaration_id);                                \
CREATE INDEX DeclRefs_ref_kind_id_idx                            \
    ON DeclRefs (ref_kind_id);                                   \
CREATE INDEX DeclRefs_line_col_idx                               \
    ON DeclRefs (source_line_id, colno);                         \
CREATE INDEX DeclRefs_is_implicit_idx                            \
    ON DeclRefs (is_implicit);                                   \
                                                                 \
CREATE TABLE SchemaInfo (                                        \
       version INTEGER                                           \
);                                                               \
                                                                 \
INSERT INTO SchemaInfo (version) VALUES (\"1\");                 \
";

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

  virtual void add_declaration(
    ASTContext *Context, CXXRecordDecl *Declaration)
  {
    FullSourceLoc FullLocation =
      Context->getFullLoc(Declaration->getLocStart());

    FileID file_id = FullLocation.getFileID();
    const FileEntry * file_entry =
      FullLocation.getManager().getFileEntryForID(file_id);
    if (! file_entry)
      return;

    std::ostringstream sql;

    sql << "BEGIN TRANSACTION;\n";

    sql << "INSERT INTO SourcePaths (dirname, pathname)"
        << "    VALUES ('" << file_entry->getDir()->getName() << "', '"
        <<              file_entry->getName() << "');\n";

    std::pair<FileID, unsigned> LocInfo = FullLocation.getDecomposedLoc();

    const llvm::MemoryBuffer *Buffer = FullLocation.getBuffer();
    const char * BufferStart = Buffer->getBufferStart();
    std::size_t offset =
      LocInfo.second - (FullLocation.getSpellingColumnNumber() - 1);

    sql << "INSERT INTO SourceLines (source_path_id, lineno, text)"
        << "    VALUES (last_insert_rowid(), "
        <<              FullLocation.getSpellingLineNumber() << ", '"
        <<              (BufferStart + offset) << "');\n";

    sql << "INSERT INTO SymbolNames (short_name, full_name)"
        << "    VALUES ('" << Declaration->getNameAsString() << "', '"
        <<              Declaration->getQualifiedNameAsString() << "');\n";

    sql << "INSERT INTO Declarations (symbol_name_id, kind_id, is_definition)"
        << "    VALUES (last_insert_rowid(), 1, 1);\n";

    sql << "INSERT INTO DeclRefs ("
        << "  declaration_id, ref_kind_id, source_line_id, colno, is_implicit)"
        << "    VALUES (last_insert_rowid(), 1,"
        << "            (SELECT COUNT(*) FROM SourceLines), "
        <<              FullLocation.getSpellingColumnNumber() << ", 0);\n";
    
    sql << "COMMIT;\n";

    std::string stmt(sql.str());
    char * error_msg;
    try {
      sql_chk(sqlite3_exec(database, stmt.c_str(), NULL, NULL, &error_msg));
    }
    catch (const std::exception& err) {
      std::cerr << "SQLite3 error: " << error_msg << std::endl;
      std::cerr << "Error occurred with the following statement: "
                << std::endl << stmt << std::endl;
      throw;
    }
  }

  virtual std::vector<TagsDeclInfo> find_declaration(const std::string& name)
  {
    std::ostringstream sql;

    sql << "\
SELECT                                            \
    Declarations.id,                              \
    SourcePaths.dirname,                          \
    SourcePaths.pathname,                         \
    SourceLines.lineno,                           \
    DeclRefs.colno,                               \
    SourceLines.text                              \
FROM                                              \
    DeclRefs,                                     \
    Declarations,                                 \
    SourcePaths,                                  \
    SourceLines                                   \
WHERE                                             \
    DeclRefs.declaration_id     = Declarations.id \
AND DeclRefs.source_line_id     = SourceLines.id  \
AND SourceLines.source_path_id  = SourcePaths.id  \
AND Declarations.symbol_name_id =                 \
    (                                             \
        SELECT                                    \
            id                                    \
        FROM                                      \
            SymbolNames                           \
        WHERE                                     \
            full_name = '" << name << "');\n";

    std::string query(sql.str());
    try {
      sqlite3_stmt *stmt;
      sql_chk(sqlite3_prepare(
                database, query.c_str(), query.length(), &stmt, NULL));

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
                << std::endl << query << std::endl;
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

  bool VisitCXXRecordDecl(CXXRecordDecl *Declaration) {
    tags_db.add_declaration(Context, Declaration);
    return true;
  }

private:
  ASTContext *Context;
};

class TagsClassConsumer : public clang::ASTConsumer
{
public:
  explicit TagsClassConsumer(ASTContext *Context, TagsDatabase& db)
    : Visitor(Context, db) {}
  virtual ~TagsClassConsumer() {}

  virtual void HandleTranslationUnit(clang::ASTContext& Context) {
    Visitor.TraverseDecl(Context.getTranslationUnitDecl());
  }

private:
  TagsClassVisitor Visitor;
};

class TagsClassAction : public clang::ASTFrontendAction
{
  SqliteTagsDatabase tags_db;

public:
  TagsClassAction(const std::string& path) : tags_db(path) {}
  virtual ~TagsClassAction() {}

  virtual clang::ASTConsumer *CreateASTConsumer(
    clang::CompilerInstance& Compiler, llvm::StringRef InFile) {
    return new TagsClassConsumer(&Compiler.getASTContext(), tags_db);
  }
};

int main(int argc, char **argv) {
  if (argc > 2) {
    if (std::string(argv[2]) == "-d") {
      SqliteTagsDatabase tags_db(argv[1]);
      std::vector<TagsDeclInfo> tags(tags_db.find_declaration(argv[3]));
      for (std::vector<TagsDeclInfo>::const_iterator i = tags.begin();
           i != tags.end();
           ++i)
        std::cout << (*i).filename << ":"
                  << (*i).line_no << ":" << (*i).col_no << ":" << (*i).text
                  << std::endl;
    } else {
      clang::tooling::runToolOnCode(new TagsClassAction(argv[1]), argv[2]);
    }
  }
}

// main.cpp ends here
