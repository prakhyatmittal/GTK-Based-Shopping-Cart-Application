/*
 * ui.c — GTK3 UI.
 *
 * Fixes vs. the original project.c:
 *   - Single application window with a GtkStack switching between
 *     Login / Signup / Shop, instead of three separate toplevel windows
 *     that were hidden (gtk_widget_hide) rather than destroyed, leaking
 *     widgets for the life of the process.
 *   - Login/signup failures now show an inline error instead of doing
 *     nothing silently.
 *   - CSS classes are actually attached to widgets via
 *     gtk_style_context_add_class(), so style.css (which the original
 *     loaded but never applied to anything) now has an effect.
 *   - Enter key submits the login/signup form (GtkEntry "activate").
 *   - Quantities are capped (MAX_QTY_PER_ITEM) instead of growing forever.
 *   - "Add All to Cart" (which only printed to stdout) is removed — the
 *     +/- stepper already *is* the cart, so that button did nothing a
 *     user could see and was actively misleading.
 *   - Checkout writes the order to the database and resets the on-screen
 *     cart afterwards. In the original, quantities were never reset, so
 *     the next checkout silently included every previous item too.
 *   - Bill text is built with GString (grows as needed) instead of a
 *     fixed 1024-byte buffer + strcat, which had no overflow protection.
 *   - Image and CSS paths are resolved relative to the executable
 *     (respath.c) instead of a hardcoded Windows path.
 */
#include "ui.h"
#include "db.h"
#include "model.h"
#include "respath.h"
#include <string.h>
#include <stdio.h>

typedef struct {
    GtkApplication *app;
    GtkWidget *window;
    GtkWidget *stack;
    GtkWidget *logout_btn;

    /* Login page */
    GtkWidget *login_username;
    GtkWidget *login_password;
    GtkWidget *login_error;

    /* Signup page */
    GtkWidget *signup_username;
    GtkWidget *signup_password;
    GtkWidget *signup_confirm;
    GtkWidget *signup_error;

    /* Shop page */
    GtkWidget *quantity_labels[NUM_ITEMS];
    GtkWidget *plus_buttons[NUM_ITEMS];
    GtkWidget *minus_buttons[NUM_ITEMS];
    GtkWidget *cart_summary_label;
    GtkWidget *cart_count_label;
    int quantities[NUM_ITEMS];

    int current_user_id;
    char current_username[128];
} AppState;

static AppState STATE;

/* ---------- small helpers ---------- */

static void set_error_label(GtkWidget *label, const char *text)
{
    gtk_label_set_text(GTK_LABEL(label), text);
    gtk_widget_set_visible(label, text != NULL && text[0] != '\0');
}

static void add_class(GtkWidget *w, const char *klass)
{
    gtk_style_context_add_class(gtk_widget_get_style_context(w), klass);
}

static GtkWidget *make_entry(gboolean is_password, const char *placeholder)
{
    GtkWidget *e = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(e), !is_password);
    gtk_entry_set_placeholder_text(GTK_ENTRY(e), placeholder);
    add_class(e, "entry-field");
    return e;
}

static void update_cart_summary(void)
{
    int total_items = 0;
    double total_price = 0.0;
    for (int i = 0; i < NUM_ITEMS; i++) {
        total_items += STATE.quantities[i];
        total_price += STATE.quantities[i] * PRODUCTS[i].price;
    }

    if (total_items == 0) {
        gtk_label_set_text(GTK_LABEL(STATE.cart_summary_label), "Your cart is empty");
        gtk_style_context_remove_class(
            gtk_widget_get_style_context(STATE.cart_summary_label), "cart-total");
        add_class(STATE.cart_summary_label, "cart-empty");
        gtk_widget_set_visible(STATE.cart_count_label, FALSE);
    } else {
        char total_buf[64];
        snprintf(total_buf, sizeof(total_buf), "\xe2\x82\xb9%.2f", total_price);
        gtk_label_set_text(GTK_LABEL(STATE.cart_summary_label), total_buf);
        gtk_style_context_remove_class(
            gtk_widget_get_style_context(STATE.cart_summary_label), "cart-empty");
        add_class(STATE.cart_summary_label, "cart-total");

        char count_buf[64];
        snprintf(count_buf, sizeof(count_buf), "%d item%s",
                 total_items, total_items == 1 ? "" : "s");
        gtk_label_set_text(GTK_LABEL(STATE.cart_count_label), count_buf);
        gtk_widget_set_visible(STATE.cart_count_label, TRUE);
    }
}

static void refresh_quantity_widget(int i)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", STATE.quantities[i]);
    gtk_label_set_text(GTK_LABEL(STATE.quantity_labels[i]), buf);
    gtk_widget_set_sensitive(STATE.plus_buttons[i], STATE.quantities[i] < MAX_QTY_PER_ITEM);
    gtk_widget_set_sensitive(STATE.minus_buttons[i], STATE.quantities[i] > 0);
    update_cart_summary();
}

static void reset_cart(void)
{
    for (int i = 0; i < NUM_ITEMS; i++) {
        STATE.quantities[i] = 0;
        refresh_quantity_widget(i);
    }
}

static void show_info_dialog(GtkMessageType type, const char *title, const char *message)
{
    GtkWidget *dialog = gtk_message_dialog_new(
        GTK_WINDOW(STATE.window), GTK_DIALOG_MODAL, type, GTK_BUTTONS_OK, "%s", message);
    gtk_window_set_title(GTK_WINDOW(dialog), title);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

/* ---------- navigation ---------- */

static void goto_page(const char *name)
{
    gtk_stack_set_visible_child_name(GTK_STACK(STATE.stack), name);
    gtk_widget_set_visible(STATE.logout_btn, strcmp(name, "shop") == 0);
}

/* ---------- callbacks: auth ---------- */

static void do_login(GtkWidget *widget, gpointer data)
{
    (void)widget; (void)data;
    const char *username = gtk_entry_get_text(GTK_ENTRY(STATE.login_username));
    const char *password = gtk_entry_get_text(GTK_ENTRY(STATE.login_password));

    if (username[0] == '\0' || password[0] == '\0') {
        set_error_label(STATE.login_error, "Enter both username and password.");
        return;
    }

    int user_id;
    if (db_authenticate(username, password, &user_id)) {
        STATE.current_user_id = user_id;
        snprintf(STATE.current_username, sizeof(STATE.current_username), "%s", username);
        set_error_label(STATE.login_error, "");
        gtk_entry_set_text(GTK_ENTRY(STATE.login_password), "");
        reset_cart();
        goto_page("shop");
    } else {
        set_error_label(STATE.login_error, "Incorrect username or password.");
    }
}

static void do_open_signup(GtkWidget *widget, gpointer data)
{
    (void)widget; (void)data;
    set_error_label(STATE.login_error, "");
    goto_page("signup");
}

static void do_open_login(GtkWidget *widget, gpointer data)
{
    (void)widget; (void)data;
    set_error_label(STATE.signup_error, "");
    goto_page("login");
}

static void do_signup(GtkWidget *widget, gpointer data)
{
    (void)widget; (void)data;
    const char *username = gtk_entry_get_text(GTK_ENTRY(STATE.signup_username));
    const char *password = gtk_entry_get_text(GTK_ENTRY(STATE.signup_password));
    const char *confirm  = gtk_entry_get_text(GTK_ENTRY(STATE.signup_confirm));

    if (username[0] == '\0' || password[0] == '\0') {
        set_error_label(STATE.signup_error, "Choose a username and password.");
        return;
    }
    if (strlen(password) < 4) {
        set_error_label(STATE.signup_error, "Password must be at least 4 characters.");
        return;
    }
    if (strcmp(password, confirm) != 0) {
        set_error_label(STATE.signup_error, "Passwords don't match.");
        return;
    }

    SignupResult result = db_create_user(username, password);
    switch (result) {
        case SIGNUP_OK:
            set_error_label(STATE.signup_error, "");
            gtk_entry_set_text(GTK_ENTRY(STATE.signup_username), "");
            gtk_entry_set_text(GTK_ENTRY(STATE.signup_password), "");
            gtk_entry_set_text(GTK_ENTRY(STATE.signup_confirm), "");
            show_info_dialog(GTK_MESSAGE_INFO, "Account created",
                              "Your account was created. You can log in now.");
            goto_page("login");
            break;
        case SIGNUP_USERNAME_TAKEN:
            set_error_label(STATE.signup_error, "That username is already taken.");
            break;
        default:
            set_error_label(STATE.signup_error, "Something went wrong. Please try again.");
            break;
    }
}

static void do_logout(GtkWidget *widget, gpointer data)
{
    (void)widget; (void)data;
    STATE.current_user_id = -1;
    STATE.current_username[0] = '\0';
    gtk_entry_set_text(GTK_ENTRY(STATE.login_username), "");
    gtk_entry_set_text(GTK_ENTRY(STATE.login_password), "");
    set_error_label(STATE.login_error, "");
    reset_cart();
    goto_page("login");
}

/* ---------- callbacks: shop ---------- */

static void on_increase(GtkWidget *w, gpointer d)
{
    (void)w;
    int i = GPOINTER_TO_INT(d);
    if (STATE.quantities[i] < MAX_QTY_PER_ITEM)
        STATE.quantities[i]++;
    refresh_quantity_widget(i);
}

static void on_decrease(GtkWidget *w, gpointer d)
{
    (void)w;
    int i = GPOINTER_TO_INT(d);
    if (STATE.quantities[i] > 0)
        STATE.quantities[i]--;
    refresh_quantity_widget(i);
}

static void do_checkout(GtkWidget *widget, gpointer data)
{
    (void)widget; (void)data;

    int total_items = 0;
    for (int i = 0; i < NUM_ITEMS; i++) total_items += STATE.quantities[i];

    if (total_items == 0) {
        show_info_dialog(GTK_MESSAGE_WARNING, "Cart is empty",
                          "Add at least one item before checking out.");
        return;
    }

    CartLine lines[NUM_ITEMS];
    int line_count = 0;
    double total_price = 0.0;

    /* The receipt is now rendered as aligned widgets from this same data,
     * so no bill string is assembled here any more. */
    for (int i = 0; i < NUM_ITEMS; i++) {
        if (STATE.quantities[i] > 0) {
            total_price += PRODUCTS[i].price * STATE.quantities[i];

            snprintf(lines[line_count].item_name, sizeof(lines[line_count].item_name),
                     "%s", PRODUCTS[i].name);
            lines[line_count].quantity = STATE.quantities[i];
            lines[line_count].price = PRODUCTS[i].price;
            line_count++;
        }
    }

    int order_id = db_record_order(STATE.current_user_id, lines, line_count, total_price);
    if (order_id < 0) {
        show_info_dialog(GTK_MESSAGE_ERROR, "Checkout failed",
                          "We couldn't save your order. Please try again.");
        return;
    }

    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        "Order confirmed", GTK_WINDOW(STATE.window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "Done", GTK_RESPONSE_OK, NULL);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 380, -1);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_box_set_spacing(GTK_BOX(content), 14);
    gtk_container_set_border_width(GTK_CONTAINER(content), 18);

    /* --- receipt panel --- */
    GtkWidget *receipt = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    add_class(receipt, "receipt");

    GtkWidget *r_title = gtk_label_new("Order confirmed");
    add_class(r_title, "receipt-title");
    gtk_label_set_xalign(GTK_LABEL(r_title), 0.0);
    gtk_box_pack_start(GTK_BOX(receipt), r_title, FALSE, FALSE, 0);

    char meta_buf[128];
    snprintf(meta_buf, sizeof(meta_buf), "Order #%d \xc2\xb7 %d item%s",
             order_id, total_items, total_items == 1 ? "" : "s");
    GtkWidget *r_meta = gtk_label_new(meta_buf);
    add_class(r_meta, "receipt-meta");
    gtk_label_set_xalign(GTK_LABEL(r_meta), 0.0);
    gtk_box_pack_start(GTK_BOX(receipt), r_meta, FALSE, FALSE, 2);

    GtkWidget *rule1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    add_class(rule1, "receipt-rule");
    gtk_box_pack_start(GTK_BOX(receipt), rule1, FALSE, FALSE, 12);

    /* One aligned row per line item: name on the left, amount on the right. */
    for (int i = 0; i < NUM_ITEMS; i++) {
        if (STATE.quantities[i] == 0) continue;

        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

        char left_buf[128];
        snprintf(left_buf, sizeof(left_buf), "%s \xc3\x97 %d",
                 PRODUCTS[i].name, STATE.quantities[i]);
        GtkWidget *left = gtk_label_new(left_buf);
        add_class(left, "receipt-line");
        gtk_label_set_xalign(GTK_LABEL(left), 0.0);

        char right_buf[32];
        snprintf(right_buf, sizeof(right_buf), "\xe2\x82\xb9%.2f",
                 PRODUCTS[i].price * STATE.quantities[i]);
        GtkWidget *right = gtk_label_new(right_buf);
        add_class(right, "receipt-line");
        gtk_label_set_xalign(GTK_LABEL(right), 1.0);

        gtk_box_pack_start(GTK_BOX(row), left, TRUE, TRUE, 0);
        gtk_box_pack_end(GTK_BOX(row), right, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(receipt), row, FALSE, FALSE, 3);
    }

    GtkWidget *rule2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    add_class(rule2, "receipt-rule");
    gtk_box_pack_start(GTK_BOX(receipt), rule2, FALSE, FALSE, 12);

    GtkWidget *total_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *t_left = gtk_label_new("Total paid");
    add_class(t_left, "receipt-total");
    gtk_label_set_xalign(GTK_LABEL(t_left), 0.0);

    char t_buf[32];
    snprintf(t_buf, sizeof(t_buf), "\xe2\x82\xb9%.2f", total_price);
    GtkWidget *t_right = gtk_label_new(t_buf);
    add_class(t_right, "receipt-total");
    gtk_label_set_xalign(GTK_LABEL(t_right), 1.0);

    gtk_box_pack_start(GTK_BOX(total_row), t_left, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(total_row), t_right, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(receipt), total_row, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(content), receipt, FALSE, FALSE, 0);

    /* --- payment QR --- */
    GtkWidget *qr_frame = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    add_class(qr_frame, "qr-frame");

    char qr_path[4096];
    respath_resolve("images/qr.jpg", qr_path, sizeof(qr_path));
    GdkPixbuf *qr_buf = gdk_pixbuf_new_from_file_at_scale(qr_path, 150, 150, TRUE, NULL);
    GtkWidget *qr = qr_buf ? gtk_image_new_from_pixbuf(qr_buf)
                           : gtk_image_new_from_icon_name("image-missing", GTK_ICON_SIZE_DIALOG);
    if (qr_buf) g_object_unref(qr_buf);
    gtk_box_pack_start(GTK_BOX(qr_frame), qr, FALSE, FALSE, 0);

    GtkWidget *qr_note = gtk_label_new("Scan to pay by UPI, or pay on delivery");
    add_class(qr_note, "qr-caption");
    gtk_box_pack_start(GTK_BOX(qr_frame), qr_note, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(content), qr_frame, FALSE, FALSE, 0);

    /* The dialog's own action button is created by GTK, so style it here
     * rather than leaving it as the stock theme's default. */
    GtkWidget *done_btn = gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog),
                                                            GTK_RESPONSE_OK);
    if (done_btn)
        add_class(done_btn, "btn-primary");

    gtk_widget_show_all(dialog);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);

    reset_cart();
}

/* ---------- page builders ---------- */

static GtkWidget *labelled_field(const char *caption, GtkWidget *entry)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *lbl = gtk_label_new(caption);
    add_class(lbl, "field-label");
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
    gtk_box_pack_start(GTK_BOX(box), lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), entry, FALSE, FALSE, 0);
    return box;
}

static GtkWidget *build_login_page(void)
{
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_halign(outer, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(outer, GTK_ALIGN_CENTER);

    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    add_class(card, "auth-card");
    gtk_widget_set_size_request(card, 360, -1);

    GtkWidget *mark = gtk_label_new("QuickCart");
    add_class(mark, "auth-mark");
    gtk_label_set_xalign(GTK_LABEL(mark), 0.0);

    GtkWidget *title = gtk_label_new("Sign in");
    add_class(title, "auth-title");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0);

    GtkWidget *subtitle = gtk_label_new("Pick up where you left off.");
    add_class(subtitle, "auth-subtitle");
    gtk_label_set_xalign(GTK_LABEL(subtitle), 0.0);

    STATE.login_username = make_entry(FALSE, "");
    STATE.login_password = make_entry(TRUE, "");

    STATE.login_error = gtk_label_new("");
    add_class(STATE.login_error, "error-label");
    gtk_widget_set_no_show_all(STATE.login_error, TRUE);
    gtk_label_set_line_wrap(GTK_LABEL(STATE.login_error), TRUE);
    gtk_label_set_xalign(GTK_LABEL(STATE.login_error), 0.0);

    GtkWidget *login_btn = gtk_button_new_with_label("Sign in");
    add_class(login_btn, "btn-primary");

    GtkWidget *signup_link = gtk_button_new_with_label("Create an account");
    add_class(signup_link, "btn-link");

    g_signal_connect(login_btn, "clicked", G_CALLBACK(do_login), NULL);
    g_signal_connect(STATE.login_username, "activate", G_CALLBACK(do_login), NULL);
    g_signal_connect(STATE.login_password, "activate", G_CALLBACK(do_login), NULL);
    g_signal_connect(signup_link, "clicked", G_CALLBACK(do_open_signup), NULL);

    gtk_box_pack_start(GTK_BOX(card), mark, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card), title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card), subtitle, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card), labelled_field("Username", STATE.login_username), FALSE, FALSE, 8);
    gtk_box_pack_start(GTK_BOX(card), labelled_field("Password", STATE.login_password), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card), STATE.login_error, FALSE, FALSE, 10);
    gtk_box_pack_start(GTK_BOX(card), login_btn, FALSE, FALSE, 14);
    gtk_box_pack_start(GTK_BOX(card), signup_link, FALSE, FALSE, 4);

    gtk_box_pack_start(GTK_BOX(outer), card, TRUE, FALSE, 0);
    return outer;
}

static GtkWidget *build_signup_page(void)
{
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_halign(outer, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(outer, GTK_ALIGN_CENTER);

    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    add_class(card, "auth-card");
    gtk_widget_set_size_request(card, 360, -1);

    GtkWidget *mark = gtk_label_new("QuickCart");
    add_class(mark, "auth-mark");
    gtk_label_set_xalign(GTK_LABEL(mark), 0.0);

    GtkWidget *title = gtk_label_new("Create an account");
    add_class(title, "auth-title");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0);

    GtkWidget *subtitle = gtk_label_new("Takes a moment. Your cart is saved to it.");
    add_class(subtitle, "auth-subtitle");
    gtk_label_set_xalign(GTK_LABEL(subtitle), 0.0);

    STATE.signup_username = make_entry(FALSE, "");
    STATE.signup_password = make_entry(TRUE, "");
    STATE.signup_confirm  = make_entry(TRUE, "");

    STATE.signup_error = gtk_label_new("");
    add_class(STATE.signup_error, "error-label");
    gtk_widget_set_no_show_all(STATE.signup_error, TRUE);
    gtk_label_set_line_wrap(GTK_LABEL(STATE.signup_error), TRUE);
    gtk_label_set_xalign(GTK_LABEL(STATE.signup_error), 0.0);

    GtkWidget *signup_btn = gtk_button_new_with_label("Create account");
    add_class(signup_btn, "btn-primary");

    GtkWidget *login_link = gtk_button_new_with_label("I already have an account");
    add_class(login_link, "btn-link");

    g_signal_connect(signup_btn, "clicked", G_CALLBACK(do_signup), NULL);
    g_signal_connect(STATE.signup_confirm, "activate", G_CALLBACK(do_signup), NULL);
    g_signal_connect(login_link, "clicked", G_CALLBACK(do_open_login), NULL);

    gtk_box_pack_start(GTK_BOX(card), mark, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card), title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card), subtitle, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card), labelled_field("Username", STATE.signup_username), FALSE, FALSE, 8);
    gtk_box_pack_start(GTK_BOX(card), labelled_field("Password", STATE.signup_password), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card), labelled_field("Confirm password", STATE.signup_confirm), FALSE, FALSE, 8);
    gtk_box_pack_start(GTK_BOX(card), STATE.signup_error, FALSE, FALSE, 8);
    gtk_box_pack_start(GTK_BOX(card), signup_btn, FALSE, FALSE, 12);
    gtk_box_pack_start(GTK_BOX(card), login_link, FALSE, FALSE, 4);

    gtk_box_pack_start(GTK_BOX(outer), card, TRUE, FALSE, 0);
    return outer;
}

static GtkWidget *build_product_card(int i)
{
    const Product *p = &PRODUCTS[i];

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    add_class(box, "product-card");
    gtk_widget_set_size_request(box, 208, -1);

    /* Photo sits on a tinted tile rather than bare white. */
    GtkWidget *thumb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    add_class(thumb, "product-thumb");
    gtk_widget_set_halign(thumb, GTK_ALIGN_FILL);

    char img_path[4096];
    respath_resolve(p->image_relpath, img_path, sizeof(img_path));
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file_at_scale(img_path, 132, 132, TRUE, NULL);
    GtkWidget *img;
    if (pixbuf) {
        img = gtk_image_new_from_pixbuf(pixbuf);
        g_object_unref(pixbuf);
    } else {
        /* Missing image falls back to a placeholder icon rather than a
         * blank square, so a broken asset never looks like a hung app. */
        img = gtk_image_new_from_icon_name("image-missing", GTK_ICON_SIZE_DIALOG);
    }
    gtk_widget_set_size_request(img, -1, 132);
    gtk_box_pack_start(GTK_BOX(thumb), img, FALSE, FALSE, 0);

    GtkWidget *name = gtk_label_new(p->name);
    add_class(name, "product-name");
    gtk_label_set_xalign(GTK_LABEL(name), 0.0);

    GtkWidget *unit = gtk_label_new(p->unit);
    add_class(unit, "product-unit");
    gtk_label_set_xalign(GTK_LABEL(unit), 0.0);

    GtkWidget *desc = gtk_label_new(p->description);
    add_class(desc, "product-desc");
    gtk_label_set_xalign(GTK_LABEL(desc), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(desc), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(desc), 26);
    gtk_label_set_lines(GTK_LABEL(desc), 2);
    gtk_label_set_ellipsize(GTK_LABEL(desc), PANGO_ELLIPSIZE_END);

    /* Price row: current price, then list price + saving only if discounted. */
    GtkWidget *price_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    char price_buf[32];
    snprintf(price_buf, sizeof(price_buf), "\xe2\x82\xb9%.0f", p->price);
    GtkWidget *price = gtk_label_new(price_buf);
    add_class(price, "product-price");
    gtk_box_pack_start(GTK_BOX(price_row), price, FALSE, FALSE, 0);

    if (p->mrp > p->price) {
        char mrp_buf[64];
        snprintf(mrp_buf, sizeof(mrp_buf),
                 "<span strikethrough='true'>\xe2\x82\xb9%.0f</span>", p->mrp);
        GtkWidget *mrp = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(mrp), mrp_buf);
        add_class(mrp, "product-mrp");
        gtk_box_pack_start(GTK_BOX(price_row), mrp, FALSE, FALSE, 0);

        char save_buf[32];
        int pct = (int)((p->mrp - p->price) / p->mrp * 100.0 + 0.5);
        snprintf(save_buf, sizeof(save_buf), "%d%% off", pct);
        GtkWidget *save = gtk_label_new(save_buf);
        add_class(save, "save-tag");
        gtk_box_pack_end(GTK_BOX(price_row), save, FALSE, FALSE, 0);
    }

    /* Quantity stepper */
    GtkWidget *stepper = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    add_class(stepper, "stepper");
    gtk_widget_set_halign(stepper, GTK_ALIGN_FILL);

    GtkWidget *minus = gtk_button_new_with_label("\xe2\x88\x92");
    GtkWidget *plus  = gtk_button_new_with_label("+");
    add_class(minus, "qty-btn");
    add_class(plus, "qty-btn");

    STATE.quantity_labels[i] = gtk_label_new("0");
    add_class(STATE.quantity_labels[i], "qty-label");
    gtk_widget_set_hexpand(STATE.quantity_labels[i], TRUE);

    STATE.plus_buttons[i] = plus;
    STATE.minus_buttons[i] = minus;
    gtk_widget_set_sensitive(minus, FALSE); /* starts at qty 0 */

    g_signal_connect(minus, "clicked", G_CALLBACK(on_decrease), GINT_TO_POINTER(i));
    g_signal_connect(plus, "clicked", G_CALLBACK(on_increase), GINT_TO_POINTER(i));

    gtk_box_pack_start(GTK_BOX(stepper), minus, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(stepper), STATE.quantity_labels[i], TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(stepper), plus, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(box), thumb, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), name, FALSE, FALSE, 10);
    gtk_box_pack_start(GTK_BOX(box), unit, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), desc, FALSE, FALSE, 6);
    gtk_box_pack_start(GTK_BOX(box), price_row, FALSE, FALSE, 8);
    gtk_box_pack_start(GTK_BOX(box), stepper, FALSE, FALSE, 4);

    return box;
}

static GtkWidget *build_shop_page(void)
{
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    /* Page heading */
    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_start(head, 20);
    gtk_widget_set_margin_end(head, 20);
    gtk_widget_set_margin_top(head, 18);
    gtk_widget_set_margin_bottom(head, 14);

    GtkWidget *title = gtk_label_new("Fresh today");
    add_class(title, "page-title");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0);

    GtkWidget *sub = gtk_label_new("Delivered to your door in under an hour.");
    add_class(sub, "page-subtitle");
    gtk_label_set_xalign(GTK_LABEL(sub), 0.0);

    gtk_box_pack_start(GTK_BOX(head), title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(head), sub, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), head, FALSE, FALSE, 0);

    /* Product grid */
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(page), scroll, TRUE, TRUE, 0);

    GtkWidget *flow = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flow), GTK_SELECTION_NONE);
    gtk_flow_box_set_activate_on_single_click(GTK_FLOW_BOX(flow), FALSE);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(flow), TRUE);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(flow), 2);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flow), 4);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(flow), 14);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(flow), 14);
    gtk_widget_set_margin_start(flow, 20);
    gtk_widget_set_margin_end(flow, 20);
    gtk_widget_set_margin_bottom(flow, 20);
    gtk_widget_set_valign(flow, GTK_ALIGN_START);
    gtk_container_add(GTK_CONTAINER(scroll), flow);

    for (int i = 0; i < NUM_ITEMS; i++)
        gtk_flow_box_insert(GTK_FLOW_BOX(flow), build_product_card(i), -1);

    /* Cart bar pinned to the bottom */
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    add_class(bar, "cart-bar");

    GtkWidget *totals = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    gtk_widget_set_valign(totals, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(totals, TRUE);

    STATE.cart_summary_label = gtk_label_new("Your cart is empty");
    add_class(STATE.cart_summary_label, "cart-empty");
    gtk_label_set_xalign(GTK_LABEL(STATE.cart_summary_label), 0.0);

    STATE.cart_count_label = gtk_label_new("");
    add_class(STATE.cart_count_label, "cart-count");
    gtk_label_set_xalign(GTK_LABEL(STATE.cart_count_label), 0.0);
    gtk_widget_set_no_show_all(STATE.cart_count_label, TRUE);

    gtk_box_pack_start(GTK_BOX(totals), STATE.cart_summary_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(totals), STATE.cart_count_label, FALSE, FALSE, 0);

    GtkWidget *checkout_btn = gtk_button_new_with_label("Checkout");
    add_class(checkout_btn, "btn-primary");
    gtk_widget_set_valign(checkout_btn, GTK_ALIGN_CENTER);
    g_signal_connect(checkout_btn, "clicked", G_CALLBACK(do_checkout), NULL);

    gtk_box_pack_start(GTK_BOX(bar), totals, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(bar), checkout_btn, FALSE, FALSE, 0);

    gtk_box_pack_end(GTK_BOX(page), bar, FALSE, FALSE, 0);

    return page;
}

/* ---------- CSS ---------- */

static void load_css(void)
{
    char css_path[4096];
    respath_resolve("style.css", css_path, sizeof(css_path));

    GtkCssProvider *provider = gtk_css_provider_new();
    GError *error = NULL;
    if (!gtk_css_provider_load_from_path(provider, css_path, &error)) {
        fprintf(stderr, "Warning: could not load style.css (%s): %s\n",
                css_path, error ? error->message : "unknown error");
        if (error) g_error_free(error);
    }
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

/* ---------- entry point ---------- */

void ui_activate(GtkApplication *app, gpointer user_data)
{
    (void)user_data;
    STATE.app = app;
    STATE.current_user_id = -1;

    load_css();

    STATE.window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(STATE.window), "QuickCart");
    gtk_window_set_default_size(GTK_WINDOW(STATE.window), 1000, 740);

    GtkWidget *header = gtk_header_bar_new();
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header), TRUE);
    gtk_header_bar_set_title(GTK_HEADER_BAR(header), "QuickCart");
    gtk_header_bar_set_subtitle(GTK_HEADER_BAR(header), "Grocery delivery");
    add_class(header, "app-header");

    STATE.logout_btn = gtk_button_new_with_label("Sign out");
    add_class(STATE.logout_btn, "btn-ghost");
    g_signal_connect(STATE.logout_btn, "clicked", G_CALLBACK(do_logout), NULL);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), STATE.logout_btn);
    gtk_widget_set_no_show_all(STATE.logout_btn, TRUE);

    gtk_window_set_titlebar(GTK_WINDOW(STATE.window), header);

    STATE.stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(STATE.stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_container_add(GTK_CONTAINER(STATE.window), STATE.stack);

    gtk_stack_add_named(GTK_STACK(STATE.stack), build_login_page(), "login");
    gtk_stack_add_named(GTK_STACK(STATE.stack), build_signup_page(), "signup");
    gtk_stack_add_named(GTK_STACK(STATE.stack), build_shop_page(), "shop");

    goto_page("login");

    gtk_widget_show_all(STATE.window);
    /* show_all above would also reveal error labels/logout button;
     * re-apply their intended visibility after. */
    set_error_label(STATE.login_error, "");
    set_error_label(STATE.signup_error, "");
    gtk_widget_set_visible(STATE.logout_btn, FALSE);
}
