/*
 * db.c — SQLite access layer.
 *
 * Fixes vs. the original project.c:
 *   1. SQL injection: the original built queries with snprintf() and raw
 *      user input ("...WHERE Username='%s' AND Password='%s'"). A username
 *      like  ' OR '1'='1  logged in as anyone. Every query here uses
 *      sqlite3_prepare_v2 + sqlite3_bind_text instead.
 *   2. Plain-text passwords: the original stored passwords as-is. Here we
 *      generate a random per-user salt and store salt + SHA-256(salt+pw).
 *   3. Silent failures: the original ignored sqlite3_exec's return value,
 *      so a duplicate username on signup failed with no feedback. This
 *      layer returns a specific result so the UI can tell the user why.
 *   4. No order history: the original never wrote a completed order
 *      anywhere — checkout just showed a dialog and the in-memory cart
 *      quantities were left untouched (so the "cart" bled into the next
 *      checkout). Orders/OrderItems tables are added and checkout resets
 *      the in-memory cart after a successful write.
 */
#include "db.h"
#include "sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static sqlite3 *db = NULL;

static void generate_salt_hex(char out_hex[33])
{
    /* 16 bytes of randomness -> 32 hex chars. Seeded once in db_init(). */
    unsigned char buf[16];
    for (int i = 0; i < 16; i++)
        buf[i] = (unsigned char)(rand() & 0xff);
    for (int i = 0; i < 16; i++)
        snprintf(out_hex + i*2, 3, "%02x", buf[i]);
}

static void hash_password(const char *password, const char *salt_hex, char out_hex[65])
{
    char combined[512];
    snprintf(combined, sizeof(combined), "%s%s", salt_hex, password);
    sha256_hex((const uint8_t *)combined, strlen(combined), out_hex);
}

bool db_init(const char *db_path)
{
    srand((unsigned)time(NULL));

    if (sqlite3_open(db_path, &db) != SQLITE_OK) {
        fprintf(stderr, "Failed to open database '%s': %s\n", db_path, sqlite3_errmsg(db));
        return false;
    }

    /* Foreign keys are off by default in SQLite; turn them on. */
    sqlite3_exec(db, "PRAGMA foreign_keys = ON;", 0, 0, 0);

    const char *schema =
        "CREATE TABLE IF NOT EXISTS Users ("
        "  Id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  Username TEXT UNIQUE NOT NULL,"
        "  PasswordHash TEXT NOT NULL,"
        "  Salt TEXT NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS Orders ("
        "  Id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  UserId INTEGER NOT NULL REFERENCES Users(Id),"
        "  Total REAL NOT NULL,"
        "  CreatedAt TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP"
        ");"
        "CREATE TABLE IF NOT EXISTS OrderItems ("
        "  Id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  OrderId INTEGER NOT NULL REFERENCES Orders(Id),"
        "  ItemName TEXT NOT NULL,"
        "  Quantity INTEGER NOT NULL,"
        "  Price REAL NOT NULL"
        ");";

    char *err = NULL;
    if (sqlite3_exec(db, schema, 0, 0, &err) != SQLITE_OK) {
        fprintf(stderr, "Schema creation failed: %s\n", err);
        sqlite3_free(err);
        return false;
    }

    return true;
}

void db_close(void)
{
    if (db) {
        sqlite3_close(db);
        db = NULL;
    }
}

bool db_authenticate(const char *username, const char *password, int *out_user_id)
{
    if (!username || !password || username[0] == '\0' || password[0] == '\0')
        return false;

    const char *sql = "SELECT Id, PasswordHash, Salt FROM Users WHERE Username = ?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return false;

    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_TRANSIENT);

    bool ok = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        int id = sqlite3_column_int(stmt, 0);
        const char *stored_hash = (const char *)sqlite3_column_text(stmt, 1);
        const char *salt = (const char *)sqlite3_column_text(stmt, 2);

        char computed[65];
        hash_password(password, salt, computed);

        if (strcmp(stored_hash, computed) == 0) {
            ok = true;
            if (out_user_id) *out_user_id = id;
        }
    }

    sqlite3_finalize(stmt);
    return ok;
}

SignupResult db_create_user(const char *username, const char *password)
{
    if (!username || !password || username[0] == '\0' || password[0] == '\0')
        return SIGNUP_ERROR;

    char salt[33];
    generate_salt_hex(salt);
    char hash[65];
    hash_password(password, salt, hash);

    const char *sql = "INSERT INTO Users (Username, PasswordHash, Salt) VALUES (?, ?, ?);";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return SIGNUP_ERROR;

    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, hash, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, salt, -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc == SQLITE_DONE)
        return SIGNUP_OK;
    if (rc == SQLITE_CONSTRAINT)
        return SIGNUP_USERNAME_TAKEN;
    return SIGNUP_ERROR;
}

int db_record_order(int user_id, const CartLine *lines, int line_count, double total)
{
    if (line_count <= 0)
        return -1;

    if (sqlite3_exec(db, "BEGIN TRANSACTION;", 0, 0, 0) != SQLITE_OK)
        return -1;

    const char *order_sql = "INSERT INTO Orders (UserId, Total) VALUES (?, ?);";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, order_sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_exec(db, "ROLLBACK;", 0, 0, 0);
        return -1;
    }
    sqlite3_bind_int(stmt, 1, user_id);
    sqlite3_bind_double(stmt, 2, total);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        sqlite3_exec(db, "ROLLBACK;", 0, 0, 0);
        return -1;
    }
    sqlite3_finalize(stmt);

    int order_id = (int)sqlite3_last_insert_rowid(db);

    const char *item_sql = "INSERT INTO OrderItems (OrderId, ItemName, Quantity, Price) VALUES (?, ?, ?, ?);";
    if (sqlite3_prepare_v2(db, item_sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_exec(db, "ROLLBACK;", 0, 0, 0);
        return -1;
    }

    for (int i = 0; i < line_count; i++) {
        sqlite3_reset(stmt);
        sqlite3_bind_int(stmt, 1, order_id);
        sqlite3_bind_text(stmt, 2, lines[i].item_name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 3, lines[i].quantity);
        sqlite3_bind_double(stmt, 4, lines[i].price);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            sqlite3_finalize(stmt);
            sqlite3_exec(db, "ROLLBACK;", 0, 0, 0);
            return -1;
        }
    }
    sqlite3_finalize(stmt);

    if (sqlite3_exec(db, "COMMIT;", 0, 0, 0) != SQLITE_OK)
        return -1;

    return order_id;
}
