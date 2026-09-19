#include "model.h"

/* mrp == price means the item is not discounted and no savings tag shows. */
const Product PRODUCTS[NUM_ITEMS] = {
    {"Apple",  "1 kg",       60.0, 72.0, "Crisp and sweet, rich in fiber.",   "images/apple.jpg"},
    {"Banana", "6 pcs",      25.0, 30.0, "Ripe robusta, full of potassium.",  "images/banana.jpg"},
    {"Milk",   "1 L pouch",  80.0, 80.0, "Toned dairy milk, pasteurised.",    "images/milk.jpg"},
    {"Bread",  "400 g loaf", 40.0, 45.0, "Soft multigrain, baked daily.",     "images/bread.jpg"},
    {"Eggs",   "Pack of 6",  42.0, 48.0, "Farm fresh, high in protein.",      "images/eggs.jpg"},
};
