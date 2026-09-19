#ifndef APP_DB_H
#define APP_DB_H

#include <sqlite3.h>
#include <stdbool.h>

typedef struct {
    int id;
    char item_name[64];
    int quantity;
    double price;
} CartLine;

/* Opens (creating if needed) the database at db_path and ensures the
 * schema exists. Returns true on success. */
bool db_init(const char *db_path);
void db_close(void);

/* Returns true and sets out_user_id if username/password match a stored,
 * hashed+salted password. Uses parameterized queries (no SQL injection). */
bool db_authenticate(const char *username, const char *password, int *out_user_id);

/* Result codes for signup, so the UI can show a precise message. */
typedef enum {
    SIGNUP_OK = 0,
    SIGNUP_USERNAME_TAKEN,
    SIGNUP_ERROR
} SignupResult;

SignupResult db_create_user(const char *username, const char *password);

/* Records a completed order (and its line items) for a user. Returns the
 * new order id, or -1 on failure. Runs as a single transaction. */
int db_record_order(int user_id, const CartLine *lines, int line_count, double total);

#endif
