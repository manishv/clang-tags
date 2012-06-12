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
#include <cassert>

#include <sqlite3.h>
#include <sys/unistd.h>

#if !defined(__has_feature) || !__has_feature(address_sanitizer)
#define HAVE_EXCEPTIONS 1
#endif

using namespace llvm;
using namespace clang;
using namespace tooling;

class TagsDeclInfo
{
public:
  int         id;
  std::string filename;
  std::string text;
  int         line_no;
  int         col_no;
  std::string name;
};

class TagsDatabase
{
public:
  virtual void add_declaration(NamedDecl *Declaration) = 0;

  virtual std::vector<TagsDeclInfo>
  find_declaration(const std::string& name) = 0;
};

inline void sql_chk(int return_code) {
#ifdef HAVE_EXCEPTIONS
  if (return_code != SQLITE_OK)
    throw std::runtime_error("SQLite3 call failed");
#endif
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
    text           TEXT,                                                \
                                                                        \
    FOREIGN KEY(source_path_id) REFERENCES SourcePaths(id)              \
);                                                                      \
CREATE UNIQUE INDEX SourceLines_id_idx                                  \
    ON SourceLines (id);                                                \
CREATE UNIQUE INDEX SourceLines_all_idx                                 \
    ON SourceLines (source_path_id, lineno);                            \
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
    symbol_name_id         TEXT NOT NULL,                               \
    kind_id                INTEGER NOT NULL,                            \
    is_definition          INTEGER NOT NULL,                            \
    is_implicitly_defined  INTEGER NOT NULL,                            \
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
CREATE INDEX DeclRefs_all_idx                                           \
    ON DeclRefs (declaration_id, ref_kind_id, source_line_id, colno,    \
                 is_implicit);                                          \
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

struct SourceLine
{
  int source_path_id;
  int lineno;

  SourceLine(int source_path_id, int lineno)
    : source_path_id(source_path_id), lineno(lineno) {}

  int compare(const SourceLine& right) const {
    int diff;
    diff = source_path_id - right.source_path_id;
    if (!diff)
      diff = lineno - right.lineno;
    return diff;
  }

  bool operator==(const SourceLine& right) const {
    return compare(right) == 0;
  }
  bool operator<(const SourceLine& right) const {
    return compare(right) < 0;
  }
};
std::map<SourceLine, int> source_lines_map;

struct SymbolName
{
  std::string short_name;
  std::string full_name;

  SymbolName(std::string short_name, std::string full_name)
    : short_name(short_name), full_name(full_name) {}

  int compare(const SymbolName& right) const {
    int diff;
    diff = short_name.compare(right.short_name);
    if (!diff)
      diff = full_name.compare(right.full_name);
    return diff;
  }

  bool operator==(const SymbolName& right) const {
    return compare(right) == 0;
  }
  bool operator<(const SymbolName& right) const {
    return compare(right) < 0;
  }
};
std::map<SymbolName, int> symbol_names_map;

struct TDeclaration
{
  int symbol_name_id;
  int kind_id;
  int is_definition;
  int is_implicitly_defined;

  TDeclaration(
    int symbol_name_id, int kind_id, int is_definition,
    int is_implicitly_defined)
    : symbol_name_id(symbol_name_id), kind_id(kind_id),
      is_definition(is_definition),
      is_implicitly_defined(is_implicitly_defined) {}

  int compare(const TDeclaration& right) const {
    int diff;
    diff = symbol_name_id - right.symbol_name_id;
    if (!diff)
      diff = kind_id - right.kind_id;
    if (!diff)
      diff = is_definition - right.is_definition;
    if (!diff)
      diff = is_implicitly_defined - right.is_implicitly_defined;
    return diff;
  }

  bool operator==(const TDeclaration& right) const {
    return compare(right) == 0;
  }
  bool operator<(const TDeclaration& right) const {
    return compare(right) < 0;
  }
};
std::map<TDeclaration, int> tdeclarations_map;

class SqliteTagsDatabase : public TagsDatabase
{
  sqlite3 *database;

  std::ostringstream pending_sql;
  int DeclarationsCounted;

public:
  explicit SqliteTagsDatabase(const std::string& path)
    : DeclarationsCounted(0)
  {
#ifdef USE_SQLITE3
    sql_chk(sqlite3_initialize());

    bool exists = false;
    if (access(path.c_str(), R_OK) == 0)
      exists = true;

#ifdef HAVE_EXCEPTIONS
    try {
#endif
      sql_chk(sqlite3_open(path.c_str(), &database));

#ifdef HAVE_EXCEPTIONS
      try {
#endif
        if (! exists) {
          char * error_msg;
#ifdef HAVE_EXCEPTIONS
          try {
#endif
            sql_chk(sqlite3_exec(
                      database, tags_sql, NULL, NULL, &error_msg));
#ifdef HAVE_EXCEPTIONS
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
#endif
#endif
  }

  virtual ~SqliteTagsDatabase() {
#ifdef USE_SQLITE3
    std::cerr << std::endl << "Executing batch SQL statements...";
    sqlite3_void_exec(pending_sql.str().c_str());
    std::cerr << "done" << std::endl;

    sqlite3_close(database);
    sqlite3_shutdown();
#endif
  }

  void sqlite3_void_exec(const char * sql)
  {
    char * error_msg;
#ifdef HAVE_EXCEPTIONS
    try {
#endif
      sql_chk(sqlite3_exec(database, sql, NULL, NULL, &error_msg));
#ifdef HAVE_EXCEPTIONS
    }
    catch (const std::exception& err) {
      std::cerr << "SQLite3 error: " << error_msg << std::endl;
      std::cerr << "Error occurred with the following statement: "
                << std::endl << sql << std::endl;
      throw;
    }
#endif
  }

  long sqlite3_query_for_id(const char * sql)
  {
    char * error_msg;
#ifdef HAVE_EXCEPTIONS
    try {
#endif
      long id = -1;
      sql_chk(sqlite3_exec(database, sql, query_callback, &id, &error_msg));
      return id;
#ifdef HAVE_EXCEPTIONS
    }
    catch (const std::exception& err) {
      std::cerr << "SQLite3 error: " << error_msg << std::endl;
      std::cerr << "Error occurred with the following statement: "
                << std::endl << sql << std::endl;
      throw;
    }
#endif
  }

  long sqlite3_insert_new(const char * sql)
  {
    char * error_msg;
#ifdef HAVE_EXCEPTIONS
    try {
#endif
      sql_chk(sqlite3_exec(database, sql, NULL, NULL, &error_msg));
      return static_cast<long>(sqlite3_last_insert_rowid(database));
#ifdef HAVE_EXCEPTIONS
    }
    catch (const std::exception& err) {
      std::cerr << "SQLite3 error: " << error_msg << std::endl;
      std::cerr << "Error occurred with the following statement: "
                << std::endl << sql << std::endl;
      throw;
    }
#endif
  }

  long sqlite3_insert_maybe(
    const char * select_sql, const char * insert_sql, ...)
  {
#ifdef USE_SQLITE3
    va_list argslist;
    va_start(argslist, insert_sql);

    char *sql = sqlite3_vmprintf(select_sql, argslist);
    long id = sqlite3_query_for_id(sql);
    sqlite3_free(sql);
    if (id == -1) {
      va_start(argslist, insert_sql);
      sql = sqlite3_vmprintf(insert_sql, argslist);
      id = sqlite3_insert_new(sql);
      sqlite3_free(sql);
    }
#else
    long id =1;
#endif
    return id;
  }

  virtual void add_declaration(NamedDecl *Declaration)
  {
    if (Declaration->getNameAsString().empty())
      return;

    ASTContext& Context(Declaration->getASTContext());

    int decl_kind_id  = 1;
    int is_implicit   = 0;
    int is_definition = 0;

    if (isa<FunctionDecl>(Declaration)) {
      if (isa<CXXConstructorDecl>(Declaration)) {
        CXXConstructorDecl * ctorDecl = cast<CXXConstructorDecl>(Declaration);
        decl_kind_id  = 1;
        is_implicit   = ctorDecl->isImplicitlyDefined();
        is_definition = ctorDecl->isThisDeclarationADefinition();
      }
      else if (isa<CXXDestructorDecl>(Declaration)) {
        CXXDestructorDecl * dtorDecl = cast<CXXDestructorDecl>(Declaration);
        decl_kind_id  = 1;
        is_implicit   = dtorDecl->isImplicitlyDefined();
        is_definition = dtorDecl->isThisDeclarationADefinition();
      }
      else {
        FunctionDecl * functionDecl = cast<FunctionDecl>(Declaration);
        decl_kind_id  = 1;
        is_definition = functionDecl->isThisDeclarationADefinition();
      }
    }
    else if (isa<TagDecl>(Declaration)) {
      TagDecl * tagDecl = cast<TagDecl>(Declaration);
      decl_kind_id  = 2;        // jww (2012-05-23): What about enums?
      is_definition = tagDecl->isThisDeclarationADefinition();
    }
    else if (isa<VarDecl>(Declaration)) {
      VarDecl * varDecl = cast<VarDecl>(Declaration);
      decl_kind_id  = 3;
      is_definition = varDecl->isThisDeclarationADefinition();
    }

    FullSourceLoc FullLocation = Context.getFullLoc(Declaration->getLocStart());
    std::pair<FileID, unsigned> LocInfo = FullLocation.getDecomposedLoc();

    FileID file_id = FullLocation.getFileID();
    const FileEntry * file_entry =
      FullLocation.getManager().getFileEntryForID(file_id);
    if (!file_entry)
      return;

    bool InvalidFile = false;
    const llvm::MemoryBuffer * Buffer =
      FullLocation.getManager().getBuffer(LocInfo.first, &InvalidFile);
    if (InvalidFile || !Buffer)
      return;

    std::size_t offset =
      LocInfo.second - (FullLocation.getSpellingColumnNumber() - 1);
    const char * BufferStart = Buffer->getBufferStart();
    const char * BufferEnd = Buffer->getBufferEnd();
    const char * LineBegin = BufferStart + offset;
#ifdef HAVE_EXCEPTIONS
    if (LineBegin >= BufferEnd)
      throw std::runtime_error("Buffer invalid");
#endif
    const char * LineEnd = LineBegin;
    while (*LineEnd && *LineEnd != '\n') {
      ++LineEnd;
#ifdef HAVE_EXCEPTIONS
      if (LineEnd >= BufferEnd)
        throw std::runtime_error("Buffer invalid");
#endif
    }
    std::string LineBuf(LineBegin, LineEnd);

    long source_path_dirname_id =
      sqlite3_insert_maybe(
        "SELECT id FROM SourcePaths WHERE pathname = %Q",
        "INSERT INTO SourcePaths (pathname) VALUES (%Q);",
        file_entry->getDir()->getName());

    long source_path_id =
      sqlite3_insert_maybe(
        "SELECT id FROM SourcePaths WHERE dirname_id = %d AND pathname = %Q",
        "INSERT INTO SourcePaths (dirname_id, pathname) VALUES (%d, %Q);",
        source_path_dirname_id, file_entry->getName());

    SourceLine source_line(
      source_path_id, FullLocation.getSpellingLineNumber());
    std::map<SourceLine, int>::iterator source_line_i =
      source_lines_map.find(source_line);

    long source_line_id;
    if (source_line_i == source_lines_map.end()) {
      source_line_id =
        sqlite3_insert_maybe(
          "SELECT id FROM SourceLines \
             WHERE source_path_id = %d AND lineno = %d",
          "INSERT INTO SourceLines (source_path_id, lineno, text) \
             VALUES (%d, %d, %Q);",
          source_line.source_path_id, source_line.lineno, LineBuf.c_str());

      source_lines_map.insert(std::make_pair(source_line, source_line_id));
    } else {
      source_line_id = (*source_line_i).second;
    }

    SymbolName symbol_name(
      Declaration->getNameAsString().c_str(),
      Declaration->getQualifiedNameAsString().c_str());
    std::map<SymbolName, int>::iterator symbol_name_i =
      symbol_names_map.find(symbol_name);

    long symbol_name_id;
    if (symbol_name_i == symbol_names_map.end()) {
      symbol_name_id =
        sqlite3_insert_maybe(
          "SELECT id FROM SymbolNames \
             WHERE short_name = %Q AND full_name = %Q",
          "INSERT INTO SymbolNames (short_name, full_name) \
             VALUES (%Q, %Q);",
          symbol_name.short_name.c_str(), symbol_name.full_name.c_str());

      symbol_names_map.insert(std::make_pair(symbol_name, symbol_name_id));
    } else {
      symbol_name_id = (*symbol_name_i).second;
    }

    TDeclaration tdeclaration(
      symbol_name_id, decl_kind_id, is_definition, is_implicit);
    std::map<TDeclaration, int>::iterator tdeclaration_i =
      tdeclarations_map.find(tdeclaration);

    long tdeclaration_id;
    if (tdeclaration_i == tdeclarations_map.end()) {
      tdeclaration_id =
        sqlite3_insert_maybe(
          "SELECT id FROM Declarations \
             WHERE symbol_name_id = %d AND kind_id = %d AND \
                   is_definition = %d AND is_implicitly_defined = %d",
          "INSERT INTO Declarations (symbol_name_id, kind_id, is_definition, \
                                     is_implicitly_defined) \
             VALUES (%d, %d, %d, %d);",
          tdeclaration.symbol_name_id, tdeclaration.kind_id,
          tdeclaration.is_definition, tdeclaration.is_implicitly_defined);

      tdeclarations_map.insert(std::make_pair(tdeclaration, tdeclaration_id));
    } else {
      tdeclaration_id = (*tdeclaration_i).second;
    }

#ifdef USE_SQLITE3
    char * sql = sqlite3_mprintf(
      "INSERT OR IGNORE INTO DeclRefs ( \
           declaration_id, ref_kind_id, source_line_id, colno, is_implicit) \
           VALUES (%d, %d, %d, %d, %d);",
      tdeclaration_id, is_definition ? 1 : 2, source_line_id,
      FullLocation.getSpellingColumnNumber(),
      tdeclaration.is_implicitly_defined);
    pending_sql << sql;
    sqlite3_free(sql);
#endif

    if (++DeclarationsCounted % 100 == 0)
      std::cerr << DeclarationsCounted << " declarations counted\r";
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
            full_name = %Q);", name.c_str());

#ifdef HAVE_EXCEPTIONS
    try {
#endif
      sqlite3_stmt *stmt;
      sql_chk(sqlite3_prepare(database, sql, std::strlen(sql), &stmt, NULL));

      std::vector<TagsDeclInfo> tags;
      while (sqlite3_step(stmt) == SQLITE_ROW) {
        TagsDeclInfo info;
        info.name     = name;
        info.id       = sqlite3_column_int(stmt, 0);
        info.filename =
          (std::string(
            reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1))) +
           "/" + reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2)));
        info.line_no  = sqlite3_column_int(stmt, 3);
        info.col_no   = sqlite3_column_int(stmt, 4);
        info.text     = (reinterpret_cast<const char *>(
                           sqlite3_column_text(stmt, 5)));
        tags.push_back(info);
      }

      sql_chk(sqlite3_finalize(stmt));
      sqlite3_free(sql);
      return tags;
#ifdef HAVE_EXCEPTIONS
    }
    catch (const std::exception& err) {
      std::cerr << "Error occurred with the following query: "
                << std::endl << sql << std::endl;
      sqlite3_free(sql);
      throw;
    }
#endif
  }
};

class TagsClassVisitor : public RecursiveASTVisitor<TagsClassVisitor>
{
  TagsDatabase& tags_db;

public:
  explicit TagsClassVisitor(TagsDatabase& db) : tags_db(db) {}
  virtual ~TagsClassVisitor() {}

  bool VisitNamedDecl(NamedDecl *Declaration) {
    tags_db.add_declaration(Declaration);
    return true;
  }
};

class TagsClassConsumer : public ASTConsumer
{
public:
  explicit TagsClassConsumer(TagsDatabase& db) : Visitor(db) {}
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

  virtual ASTConsumer *CreateASTConsumer(CompilerInstance&, llvm::StringRef) {
    return new TagsClassConsumer(db);
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
