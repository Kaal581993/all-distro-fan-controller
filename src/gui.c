#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static GtkWidget *window, *model, *cpu, *fan, *gpu, *state_label, *detail, *notice;
static GtkWidget *profile_buttons[4], *fan_auto, *fan_full, *scale, *cap_apply;
static GtkWidget *boost, *battery_profile, *release_button, *start_button;
static char *client_path, *test_socket, *screenshot_path;
static char *controller_error;
static gboolean busy, updating, slider_dirty, have_status, smoke_test, controller_starting;
static GHashTable *values;
static const char *ids[] = {"power-saving", "medium", "performance", "automatic"};
static const char *names[] = {"Power Saving", "Medium Throttle", "Performance", "Automatic"};
static gboolean refresh(gpointer unused);
static void start_controller(GtkButton *button, gpointer unused);

static const char *value(const char *key)
{
    const char *result = g_hash_table_lookup(values, key);
    return result ? result : "";
}

static int integer(const char *key)
{
    const char *text = value(key);
    return *text ? (int)g_ascii_strtoll(text, NULL, 10) : -1;
}

static const char *profile_name(const char *id)
{
    for (size_t i = 0; i < G_N_ELEMENTS(ids); ++i)
        if (!strcmp(id, ids[i])) return names[i];
    return "Firmware control";
}

static void label_text(GtkWidget *label, const char *text)
{
    gtk_label_set_text(GTK_LABEL(label), text);
}

static void set_metric(GtkWidget *label, const char *heading, int number, gboolean temperature)
{
    char *measurement = number < 0 ? g_strdup("Unavailable") :
        temperature ? g_strdup_printf("%.1f °C", number / 1000.0) : g_strdup_printf("%d RPM", number);
    char *markup = g_markup_printf_escaped("<span size='small'>%s</span>\n<span size='xx-large' weight='bold'>%s</span>",
                                          heading, measurement);
    gtk_label_set_markup(GTK_LABEL(label), markup);
    g_free(markup);
    g_free(measurement);
}

static void render(const char *response)
{
    g_hash_table_remove_all(values);
    char **lines = g_strsplit(response, "\n", -1);
    for (size_t i = 0; lines[i]; ++i) {
        char *equals = strchr(lines[i], '=');
        if (!equals) continue;
        *equals++ = '\0';
        g_hash_table_insert(values, g_strdup(lines[i]), g_strdup(equals));
    }
    g_strfreev(lines);
    have_status = *value("daemon") != '\0';
    gboolean online = !strcmp(value("daemon"), "online");
    gboolean active = online && integer("enabled") == 1;
    updating = TRUE;
    label_text(model, value("model"));
    set_metric(cpu, "CPU temperature", integer("cpu_temp"), TRUE);
    set_metric(fan, "CPU / system fan", integer("cpu_rpm"), FALSE);
    set_metric(gpu, "GPU temperature", integer("gpu_temp"), TRUE);
    for (size_t i = 0; i < G_N_ELEMENTS(profile_buttons); ++i) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(profile_buttons[i]),
                                     active && !strcmp(value("profile"), ids[i]));
        gtk_widget_set_sensitive(profile_buttons[i], !controller_starting &&
            (i == 3 || integer("cpu_controls_locked") != 1));
        gtk_widget_set_tooltip_text(profile_buttons[i], i < 3 && integer("cpu_controls_locked") == 1 ?
            "CPU power files are locked by the operating system. The fan buttons above work independently." : NULL);
    }
    const char *source = integer("ac") == 1 ? "Plugged in" :
                         integer("ac") == 0 ? "On battery" : "Power source unknown";
    char *text = g_strdup_printf("%s  •  %s  •  %s", source,
        online ? "Fan controller running" : controller_starting ? "Starting user fan controller…" : "Fan controller stopped",
        active ? integer("cpu_controls_locked") == 1 ?
            (integer("cpu_fan_mode") == 0 ? "Maximum fan" : "Automatic cooling") :
            profile_name(value("effective")) : "Firmware control");
    label_text(state_label, text);
    g_free(text);
    int cap = integer("actual_cap");
    int rpm = integer("cpu_peak_rpm");
    char *cap_text = cap < 0 ? g_strdup("unavailable") : g_strdup_printf("%d%%", cap);
    char *peak_text = rpm <= 0 ? g_strdup("unavailable") : g_strdup_printf("%d RPM", rpm);
    char *gpu_fan = integer("gpu_fan_control") == 1 ?
        g_strdup_printf("GPU fan: %d RPM", integer("gpu_rpm")) :
        g_strdup("Separate GPU fan control: not exposed by this laptop");
    text = g_strdup_printf(
        "CPU ceiling: %s  •  Turbo: %s  •  Firmware profile: %s\n"
        "Fan mode: %s  •  Observed peak: %s (not a rated maximum)\n"
        "Requested range: 7,900–8,000 RPM; firmware maximum mode only, exact RPM control unavailable.\n%s",
        cap_text, integer("no_turbo") == 0 ? "enabled" : integer("no_turbo") == 1 ? "disabled" : "unavailable",
        value("platform"), integer("cpu_fan_mode") == 0 ? "Firmware maximum" :
        integer("cpu_fan_mode") == 2 ? "Firmware automatic" : "Unavailable",
        peak_text, gpu_fan);
    label_text(detail, text);
    g_free(text); g_free(cap_text); g_free(peak_text); g_free(gpu_fan);
    if (!online) label_text(notice, controller_error ? controller_error : controller_starting ?
        "Starting fan control under your desktop account. No administrator prompt is needed." :
        "Click Maximum fan or Start controls to start your user controller. No administrator prompt.");
    else if (*value("error")) label_text(notice, value("error"));
    else if (!active) label_text(notice,
        "Monitoring only. Select a profile to resume fan control and the temperature override.");
    else if (integer("guard") > 0 && integer("cpu_controls_locked") == 1) label_text(notice,
        "High CPU temperature: maximum fan is requested. CPU throttling is managed by the operating system.\n"
        "The fan controls run as your desktop user; protected CPU settings remain unavailable.");
    else if (integer("guard") == 2) label_text(notice,
        "Temperature override: maximum cooling, turbo off, CPU ceiling at most 40%.\n"
        "Triggered at 90 °C or when CPU temperature is unavailable.");
    else if (integer("guard") == 1) label_text(notice,
        "Temperature override: maximum cooling, turbo off, CPU ceiling at most 60%.\n"
        "Your selected profile resumes after cooling to 78 °C.");
    else if (integer("ac") == 1 && integer("cpu_controls_locked") == 1) label_text(notice,
        "Plugged in: maximum fan is requested continuously under your desktop account.\n"
        "CPU power settings are managed by the operating system. They do not block fan control.");
    else if (integer("ac") == 1) label_text(notice,
        "Plugged in: maximum fan is requested continuously across every profile.\n"
        "Automatic mode keeps Performance selected. Fan RPM shown above is measured.");
    else if (integer("fan_temperature_boost") == 1) label_text(notice,
        "80 °C cooling trigger: maximum fan remains requested until cooling to 75 °C.\n"
        "CPU throttling begins separately at 85 °C if needed.");
    else if (!strcmp(value("profile"), "performance") && integer("ac") != 1)
        label_text(notice, "Performance will resume when plugged in. Power Saving is active on battery.");
    else label_text(notice,
        "Profiles apply immediately and are remembered. Closing this window leaves the service running.");
    gboolean fans_available = !controller_starting &&
        (integer("cpu_fan_write_access") == 1 || integer("gpu_fan_write_access") == 1);
    gtk_widget_set_sensitive(fan_auto, fans_available && integer("ac") != 1 &&
                             integer("fan_temperature_boost") != 1 && integer("guard") < 1);
    gtk_widget_set_tooltip_text(fan_auto,
        "Automatic fan mode is available on battery after the temperature override clears.");
    gtk_widget_set_sensitive(fan_full, fans_available);
    gtk_widget_set_sensitive(scale, !controller_starting && integer("intel_control") == 1);
    gtk_widget_set_sensitive(cap_apply, gtk_widget_get_sensitive(scale));
    gtk_widget_set_sensitive(boost, gtk_widget_get_sensitive(scale));
    gtk_widget_set_tooltip_text(scale, integer("cpu_controls_locked") == 1 ?
        "The operating system protects CPU performance settings. Fan controls remain available without administrator access." : NULL);
    gtk_widget_set_sensitive(battery_profile, !controller_starting);
    gtk_widget_set_sensitive(release_button, online);
    gtk_widget_set_sensitive(start_button, !controller_starting && !online);
    gtk_button_set_label(GTK_BUTTON(start_button), online ? "Controls running" : "Start controls");
    gtk_button_set_label(GTK_BUTTON(profile_buttons[3]), integer("cpu_controls_locked") == 1 ?
        "Automatic cooling — maximum on AC, 80 °C trigger on battery" :
        "Automatic — switch profiles when plugging / unplugging power");
    if (!slider_dirty) gtk_range_set_value(GTK_RANGE(scale),
        integer("requested_cap") > 0 ? integer("requested_cap") : cap > 0 ? cap : 70);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(boost),
        integer("requested_boost") == 0 ? "off" : integer("requested_boost") == 1 ? "on" : "default");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(battery_profile), value("battery_profile"));
    updating = FALSE;
}

static void status_finished(GObject *source, GAsyncResult *result, gpointer unused)
{
    (void)unused;
    char *output = NULL, *errors = NULL;
    GError *error = NULL;
    gboolean ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &output, &errors, &error);
    busy = FALSE;
    if (ok && g_subprocess_get_successful(G_SUBPROCESS(source))) render(output);
    else label_text(notice, error ? error->message : errors && *errors ? errors : "Cannot read hardware status.");
    g_clear_error(&error);
    g_free(output); g_free(errors);
}

static GSubprocess *spawn_client(const char *first, const char *second, GError **error)
{
    const char *argv[8];
    size_t i = 0;
    argv[i++] = client_path;
    if (test_socket) { argv[i++] = "--socket"; argv[i++] = test_socket; }
    argv[i++] = first;
    if (second) argv[i++] = second;
    argv[i] = NULL;
    return g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE, error);
}

static gboolean refresh(gpointer unused)
{
    (void)unused;
    if (busy) return G_SOURCE_CONTINUE;
    GError *error = NULL;
    GSubprocess *process = spawn_client("status", NULL, &error);
    if (!process) {
        label_text(notice, error->message);
        g_error_free(error);
        return G_SOURCE_CONTINUE;
    }
    busy = TRUE;
    g_subprocess_communicate_utf8_async(process, NULL, NULL, status_finished, NULL);
    g_object_unref(process);
    return G_SOURCE_CONTINUE;
}

static void command_finished(GObject *source, GAsyncResult *result, gpointer unused)
{
    (void)unused;
    char *output = NULL, *errors = NULL;
    GError *error = NULL;
    gboolean ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &output, &errors, &error);
    if (!ok || !g_subprocess_get_successful(G_SUBPROCESS(source))) {
        GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(window), GTK_DIALOG_MODAL,
            GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s",
            error ? error->message : errors && *errors ? errors : output && *output ? output : "Control failed.");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
    } else slider_dirty = FALSE;
    g_clear_error(&error); g_free(output); g_free(errors);
    refresh(NULL);
}

static void send_command(const char *first, const char *second)
{
    if (updating) return;
    GError *error = NULL;
    GSubprocess *process = spawn_client(first, second, &error);
    if (!process) { label_text(notice, error->message); g_error_free(error); return; }
    g_subprocess_communicate_utf8_async(process, NULL, NULL, command_finished, NULL);
    g_object_unref(process);
}

static void clicked(GtkButton *button, gpointer unused)
{
    (void)unused;
    const char *action = g_object_get_data(G_OBJECT(button), "action");
    char **words = g_strsplit(action, " ", 2);
    send_command(words[0], words[1]);
    g_strfreev(words);
}

static void apply_cap(GtkButton *button, gpointer unused)
{
    (void)button; (void)unused;
    char cap[8];
    g_snprintf(cap, sizeof cap, "%d", (int)gtk_range_get_value(GTK_RANGE(scale)));
    send_command("cap", cap);
}

static void dirty_cap(GtkRange *range, gpointer unused)
{
    (void)range; (void)unused;
    if (!updating) slider_dirty = TRUE;
}

static void changed(GtkComboBox *combo, gpointer action)
{
    const char *id = gtk_combo_box_get_active_id(combo);
    if (id) send_command(action, id);
}

static void installation_help(GtkButton *button, gpointer unused)
{
    (void)button; (void)unused;
    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(window), GTK_DIALOG_MODAL,
        GTK_MESSAGE_INFO, GTK_BUTTONS_CLOSE,
        "Fan controls run as your normal desktop user.\n\n"
        "Click Maximum fan, Automatic cooling, or Start controls. The user controller starts automatically.\n"
        "No administrator password is requested for fan operation.\n\n"
        "This laptop exposes Automatic and Maximum modes. It does not expose a numeric RPM target.\n"
        "The RPM reading is the speed the firmware actually reaches.\n\n"
        "CPU power files can remain protected by the operating system. Their permissions do not block fan control.\n"
        "If fan access is unavailable, see ~/.local/state/fan-control-center/controller.log.");
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

static void controller_started(GObject *source, GAsyncResult *result, gpointer unused)
{
    (void)unused;
    char *output = NULL, *errors = NULL;
    GError *error = NULL;
    gboolean ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &output, &errors, &error);
    controller_starting = FALSE;
    g_clear_pointer(&controller_error, g_free);
    if (!ok || !g_subprocess_get_successful(G_SUBPROCESS(source)))
        controller_error = g_strdup(error ? error->message :
            errors && *errors ? errors : "User controller could not start. Check the controller log.");
    g_clear_error(&error); g_free(output); g_free(errors);
    refresh(NULL);
}

static void start_controller(GtkButton *button, gpointer unused)
{
    (void)button; (void)unused;
    if (controller_starting || test_socket) return;
    GError *error = NULL;
    GSubprocess *process = spawn_client("--start-user-daemon", NULL, &error);
    if (!process) { label_text(notice, error->message); g_error_free(error); return; }
    controller_starting = TRUE;
    g_clear_pointer(&controller_error, g_free);
    g_subprocess_communicate_utf8_async(process, NULL, NULL, controller_started, NULL);
    g_object_unref(process);
}

static void show_monitor(GtkButton *button, gpointer unused)
{
    (void)button; (void)unused;
    char *directory = g_path_get_dirname(client_path);
    char *monitor = g_build_filename(directory, "fan-monitor", NULL);
    GError *error = NULL;
    GSubprocess *process = g_subprocess_new(G_SUBPROCESS_FLAGS_NONE, &error, monitor, NULL);
    if (process) g_object_unref(process);
    else { label_text(notice, error->message); g_error_free(error); }
    g_free(directory); g_free(monitor);
}

static GtkWidget *plain_label(const char *text)
{
    GtkWidget *label = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    return label;
}

static GtkWidget *action_button(const char *text, const char *action)
{
    GtkWidget *button = gtk_button_new_with_label(text);
    g_object_set_data_full(G_OBJECT(button), "action", g_strdup(action), g_free);
    g_signal_connect(button, "clicked", G_CALLBACK(clicked), NULL);
    return button;
}

static GtkWidget *profile_combo(int count)
{
    GtkWidget *combo = gtk_combo_box_text_new();
    for (int i = 0; i < count; ++i)
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), ids[i], names[i]);
    return combo;
}

static gboolean finish_smoke(gpointer unused)
{
    (void)unused;
    if (!have_status) { g_printerr("GUI did not receive hardware status\n"); exit(1); }
    if (screenshot_path) {
        GtkAllocation allocation;
        gtk_widget_get_allocation(window, &allocation);
        GdkPixbuf *pixels = gdk_pixbuf_get_from_window(gtk_widget_get_window(window),
                                                     0, 0, allocation.width, allocation.height);
        GError *error = NULL;
        if (!pixels || !gdk_pixbuf_save(pixels, screenshot_path, "png", &error, NULL)) {
            g_printerr("Screenshot failed: %s\n", error ? error->message : "no pixels");
            g_clear_error(&error);
            exit(1);
        }
        g_object_unref(pixels);
    }
    g_print("GUI smoke test: profile buttons, hardware status and power selectors rendered.\n");
    gtk_main_quit();
    return G_SOURCE_REMOVE;
}

int main(int argc, char **argv)
{
    if (geteuid() == 0) { g_printerr("Run the GUI as your desktop user.\n"); return 1; }
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--socket") && i + 1 < argc) test_socket = g_strdup(argv[++i]);
        else if (!strcmp(argv[i], "--smoke-test")) smoke_test = TRUE;
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) screenshot_path = g_strdup(argv[++i]);
        else { g_printerr("Unknown GUI option: %s\n", argv[i]); return 1; }
    }
    if (!gtk_init_check(NULL, NULL)) { g_printerr("Start Fan Control Center in your graphical desktop session.\n"); return 1; }
    GError *error = NULL;
    char *exe = g_file_read_link("/proc/self/exe", &error);
    if (!exe) { g_printerr("%s\n", error->message); g_error_free(error); return 1; }
    char *directory = g_path_get_dirname(exe);
    client_path = g_build_filename(directory, "fanctl", NULL);
    g_free(exe); g_free(directory);
    values = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css,
        "window { background: #111827; color: #eef2f7; }"
        "label { color: #eef2f7; }"
        "button { padding: 10px; }"
        "button.profile { background: #1f2937; color: #eef2f7; border: 2px solid #374151; border-radius: 12px; }"
        "button.profile:checked { background: #134e4a; border-color: #2dd4bf; }"
        "button.profile:disabled { opacity: 0.65; }"
        ".notice { background: #1f2937; padding: 12px; border-radius: 8px; }", -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Fan Control Center");
    gtk_window_set_icon_name(GTK_WINDOW(window), "fan-control-center");
    gtk_window_set_default_size(GTK_WINDOW(window), 810, 610);
    gtk_container_set_border_width(GTK_CONTAINER(window), 24);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_container_add(GTK_CONTAINER(window), box);
    GtkWidget *title = plain_label(NULL);
    gtk_label_set_markup(GTK_LABEL(title), "<span size='xx-large' weight='bold'>Fan Control Center</span>");
    gtk_box_pack_start(GTK_BOX(box), title, FALSE, FALSE, 0);
    model = plain_label("Reading laptop sensors…");
    gtk_box_pack_start(GTK_BOX(box), model, FALSE, FALSE, 0);
    GtkWidget *meters = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 24);
    gtk_box_set_homogeneous(GTK_BOX(meters), TRUE);
    cpu = plain_label("CPU temperature"); fan = plain_label("Fan RPM"); gpu = plain_label("GPU temperature");
    gtk_box_pack_start(GTK_BOX(meters), cpu, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(meters), fan, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(meters), gpu, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), meters, FALSE, FALSE, 0);
    GtkWidget *fan_controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    fan_full = action_button("Maximum fan", "fan full");
    fan_auto = action_button("Firmware Auto (battery)", "fan auto");
    GtkWidget *fan_default = action_button("Use automatic cooling rules", "fan default");
    gtk_box_pack_start(GTK_BOX(fan_controls), fan_full, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(fan_controls), fan_auto, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(fan_controls), fan_default, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), fan_controls, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box),
        plain_label("Fan modes: Automatic / Maximum. Exact RPM adjustment is not exposed by this laptop."), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), plain_label("Optional CPU power profiles"), FALSE, FALSE, 0);
    GtkWidget *cards = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_set_homogeneous(GTK_BOX(cards), TRUE);
    const char *descriptions[] = {
        "Lower heat & battery use\n45% CPU ceiling · Turbo off",
        "Everyday work\n70% CPU ceiling · Turbo off",
        "For plugged-in workloads\n100% ceiling · Maximum fan"
    };
    for (int i = 0; i < 3; ++i) {
        profile_buttons[i] = gtk_toggle_button_new();
        gtk_widget_set_name(profile_buttons[i], ids[i]);
        gtk_style_context_add_class(gtk_widget_get_style_context(profile_buttons[i]), "profile");
        char *markup = g_markup_printf_escaped("<b>%s</b>\n\n%s", names[i], descriptions[i]);
        GtkWidget *label = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(label), markup);
        gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);
        gtk_container_add(GTK_CONTAINER(profile_buttons[i]), label);
        char *action = g_strdup_printf("profile %s", ids[i]);
        g_object_set_data_full(G_OBJECT(profile_buttons[i]), "action", action, g_free);
        g_signal_connect(profile_buttons[i], "clicked", G_CALLBACK(clicked), NULL);
        gtk_box_pack_start(GTK_BOX(cards), profile_buttons[i], TRUE, TRUE, 0);
        g_free(markup);
    }
    gtk_box_pack_start(GTK_BOX(box), cards, FALSE, FALSE, 0);
    profile_buttons[3] = gtk_toggle_button_new_with_label("Automatic — switch profiles when plugging / unplugging power");
    g_object_set_data(G_OBJECT(profile_buttons[3]), "action", "profile automatic");
    g_signal_connect(profile_buttons[3], "clicked", G_CALLBACK(clicked), NULL);
    gtk_box_pack_start(GTK_BOX(box), profile_buttons[3], FALSE, FALSE, 0);
    state_label = plain_label("Connecting to the service…");
    gtk_box_pack_start(GTK_BOX(box), state_label, FALSE, FALSE, 0);
    notice = plain_label("");
    gtk_style_context_add_class(gtk_widget_get_style_context(notice), "notice");
    gtk_box_pack_start(GTK_BOX(box), notice, FALSE, FALSE, 0);
    GtkWidget *expander = gtk_expander_new("Manual fan, throttle & power-source settings");
    GtkWidget *advanced = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(advanced), 12);
    gtk_grid_set_column_spacing(GTK_GRID(advanced), 12);
    gtk_container_add(GTK_CONTAINER(expander), advanced);
    gtk_grid_attach(GTK_GRID(advanced), plain_label("CPU ceiling (%)"), 0, 1, 1, 1);
    scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 20, 100, 5);
    gtk_scale_set_digits(GTK_SCALE(scale), 0);
    gtk_widget_set_hexpand(scale, TRUE);
    gtk_grid_attach(GTK_GRID(advanced), scale, 1, 1, 1, 1);
    g_signal_connect(scale, "value-changed", G_CALLBACK(dirty_cap), NULL);
    cap_apply = gtk_button_new_with_label("Apply CPU ceiling");
    g_signal_connect(cap_apply, "clicked", G_CALLBACK(apply_cap), NULL);
    gtk_grid_attach(GTK_GRID(advanced), cap_apply, 2, 1, 1, 1);
    boost = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(boost), "default", "Turbo: profile default");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(boost), "off", "Turbo: disabled");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(boost), "on", "Turbo: enabled");
    g_signal_connect(boost, "changed", G_CALLBACK(changed), "boost");
    gtk_grid_attach(GTK_GRID(advanced), boost, 1, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(advanced), action_button("Reset CPU ceiling", "cap default"), 2, 2, 1, 1);
    battery_profile = profile_combo(2);
    g_signal_connect(battery_profile, "changed", G_CALLBACK(changed), "battery-profile");
    gtk_grid_attach(GTK_GRID(advanced), plain_label("Automatic: plugged in"), 0, 3, 1, 1);
    gtk_grid_attach(GTK_GRID(advanced), plain_label("Performance (fixed) · maximum fan"), 1, 3, 2, 1);
    gtk_grid_attach(GTK_GRID(advanced), plain_label("Automatic: battery"), 0, 4, 1, 1);
    gtk_grid_attach(GTK_GRID(advanced), battery_profile, 1, 4, 2, 1);
    detail = plain_label("");
    gtk_grid_attach(GTK_GRID(advanced), detail, 0, 5, 3, 1);
    gtk_box_pack_start(GTK_BOX(box), expander, FALSE, FALSE, 0);
    GtkWidget *footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *help = gtk_button_new_with_label("Fan control help");
    g_signal_connect(help, "clicked", G_CALLBACK(installation_help), NULL);
    release_button = action_button("Restore firmware control", "release");
    gtk_box_pack_start(GTK_BOX(footer), help, FALSE, FALSE, 0);
    start_button = gtk_button_new_with_label("Start controls");
    g_signal_connect(start_button, "clicked", G_CALLBACK(start_controller), NULL);
    gtk_box_pack_start(GTK_BOX(footer), start_button, FALSE, FALSE, 0);
    GtkWidget *monitor_button = gtk_button_new_with_label("Show taskbar fan monitor");
    g_signal_connect(monitor_button, "clicked", G_CALLBACK(show_monitor), NULL);
    gtk_box_pack_start(GTK_BOX(footer), monitor_button, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(footer), release_button, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(box), footer, FALSE, FALSE, 0);
    gtk_widget_show_all(window);
    if (!smoke_test) start_controller(NULL, NULL);
    refresh(NULL);
    g_timeout_add_seconds(2, refresh, NULL);
    if (smoke_test) g_timeout_add(1800, finish_smoke, NULL);
    gtk_main();
    g_hash_table_unref(values);
    g_free(client_path); g_free(test_socket); g_free(screenshot_path); g_free(controller_error);
    return 0;
}
