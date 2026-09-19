#ifndef APP_MODEL_H
#define APP_MODEL_H

/*
 * One struct per product, instead of the original's four parallel arrays
 * (items / prices / descriptions / item_images) matched only by index.
 */

#define NUM_ITEMS 5
#define MAX_QTY_PER_ITEM 20   /* the original had no upper bound at all */

typedef struct {
    const char *name;
    const char *unit;           /* pack size, e.g. "1 kg" — retail context */
    double price;
    double mrp;                 /* list price; > price means it's on offer */
    const char *description;
    const char *image_relpath;  /* relative to the executable's directory */
} Product;

extern const Product PRODUCTS[NUM_ITEMS];

#endif
