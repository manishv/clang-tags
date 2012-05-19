CREATE TABLE SourcePaths (
       id INTEGER PRIMARY KEY,

       pathname VARCHAR(4096) NOT NULL
);

CREATE TABLE SourceLines (
       id INTEGER PRIMARY KEY,

       source_path_id INTEGER NOT NULL,
       lineno         INTEGER NOT NULL,
       text           TEXT NOT NULL,

       FOREIGN KEY(source_path_id) REFERENCES SourcePaths(id)
);

CREATE TABLE DeclKinds (
       id INTEGER PRIMARY KEY,

       description CHAR(40) NOT NULL
);

INSERT INTO DeclKinds (description) VALUES ("function");
INSERT INTO DeclKinds (description) VALUES ("type");
INSERT INTO DeclKinds (description) VALUES ("variable");
INSERT INTO DeclKinds (description) VALUES ("enum");
INSERT INTO DeclKinds (description) VALUES ("macro");
INSERT INTO DeclKinds (description) VALUES ("namespace");

CREATE TABLE SymbolNames (
       id INTEGER PRIMARY KEY,

       full_name  TEXT NOT NULL
);

CREATE TABLE Declarations (
       id INTEGER PRIMARY KEY,

       symbol_name_id TEXT NOT NULL,
       kind_id        INTEGER NOT NULL,
       is_definition  INTEGER NOT NULL,

       FOREIGN KEY(symbol_name_id) REFERENCES SymbolNames(id),
       FOREIGN KEY(kind_id)        REFERENCES DeclKinds(id)
);

CREATE TABLE DeclRefKinds (
       id INTEGER PRIMARY KEY,

       description CHAR(40) NOT NULL
);

INSERT INTO DeclRefKinds (description) VALUES ("definition");
INSERT INTO DeclRefKinds (description) VALUES ("declaration");
INSERT INTO DeclRefKinds (description) VALUES ("use");

CREATE TABLE DeclRefs (
       id INTEGER PRIMARY KEY,

       declaration_id INTEGER NOT NULL,
       ref_kind_id    INTEGER NOT NULL,
       source_line_id INTEGER NOT NULL,
       colno          INTEGER NOT NULL,
       is_implicit    INTEGER NOT NULL,

       context_ref_id INTEGER,  -- the nearest enclosing entity

       FOREIGN KEY(declaration_id) REFERENCES Entities(id),
       FOREIGN KEY(ref_kind_id)    REFERENCES DeclRefKinds(id),
       FOREIGN KEY(source_line_id) REFERENCES SourceLines(id)
       FOREIGN KEY(context_ref_id) REFERENCES DeclRefs(id)
);

CREATE TABLE SchemaInfo (
       VERSION INTEGER
);