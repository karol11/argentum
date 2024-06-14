#include <stdio.h>
#include "runtime.h"
#include "blob.h"
#include "sqlite3.h"

typedef struct {
    AgObject header;
    sqlite3* con;
} AgSqlite;

typedef struct {
    AgObject header;
    sqlite3_stmt* stmt;
} AgSqliteQuery;

AgSqlite* ag_m_sqliteFfi_Sqlite_sqliteFfi_open(
    AgSqlite* con,
    AgString* file_name,
    int flags)
{
    int r = sqlite3_open_v2(file_name->chars, &con->con, flags, NULL);
    if (r != SQLITE_OK) {
        fprintf(stderr, "sqlite3_open_v2 %s", sqlite3_errstr(r));
        return NULL;
    }
    ag_retain_pin_nn(&con->header);
    return con;
}

void ag_fn_sqliteFfi_disposeSqlite(AgSqlite* con) {
    if (con->con)
        sqlite3_close_v2(con->con);
}

AgSqliteQuery* ag_m_sqliteFfi_Sqlite_sqliteFfi_internalQuery(
    AgSqlite* con,
    AgSqliteQuery* q,
    AgString* sql,
    int flags)
{
    if (sqlite3_prepare_v3(con->con, sql->chars, -1, flags, &q->stmt, NULL) != SQLITE_OK)
        con->con = NULL;
    ag_retain_pin_nn(&q->header);
    return q;
}

void ag_fn_sqliteFfi_disposeRow(AgSqliteQuery* q) {
    if (q->stmt)
        sqlite3_finalize(q->stmt);
}

int64_t ag_m_sqliteFfi_Row_sqliteFfi_intAt(AgSqliteQuery* q, int at) {
    return sqlite3_column_int64(q->stmt, at);
}

double ag_m_sqliteFfi_Row_sqliteFfi_doubleAt(AgSqliteQuery* q, int at) {
    return sqlite3_column_double(q->stmt, at);
}

AgString* ag_m_sqliteFfi_Row_sqliteFfi_stringAt(AgSqliteQuery* q, int at) {
    return ag_make_str(
        sqlite3_column_text(q->stmt, at),
        sqlite3_column_bytes(q->stmt, at));
}

void ag_m_sqliteFfi_Row_sqliteFfi_blobAt(AgSqliteQuery* q, int at, AgBlob* result) {
    const void* r = sqlite3_column_blob(q->stmt, at);
    int64_t size = r
        ? sqlite3_column_bytes(q->stmt, at)
        : 0;
    ag_make_blob_fit(result, size);
    ag_memcpy(result->bytes, r, size);
}

AgSqliteQuery* ag_m_sqliteFfi_Query_sqliteFfi_setString(AgSqliteQuery* q, int at, AgString* val) {
    sqlite3_bind_text(q->stmt, at, val->chars, (int) strlen(val->chars), SQLITE_TRANSIENT);  // TODO: optimize after String/Cursor refactoring
    ag_retain_pin_nn(&q->header);
    return q;
}

AgSqliteQuery* ag_m_sqliteFfi_Query_sqliteFfi_setBlob(AgSqliteQuery* q, int at, AgBlob* val) {
    sqlite3_bind_blob(q->stmt, at, val->bytes, (int) val->bytes_count, SQLITE_TRANSIENT); // It's mutable, so copy
    ag_retain_pin_nn(&q->header);
    return q;
}

AgSqliteQuery* ag_m_sqliteFfi_Query_sqliteFfi_setInt(AgSqliteQuery* q, int at, int64_t val) {
    sqlite3_bind_int64(q->stmt, at, val);
    ag_retain_pin_nn(&q->header);
    return q;
}

bool ag_m_sqliteFfi_Query_sqliteFfi_internalStep(AgSqliteQuery* q) {
    if (sqlite3_step(q->stmt) == SQLITE_ROW)
        return true;
    // TODO: add error handling/logging
    sqlite3_reset(q->stmt);
    return false;
}
