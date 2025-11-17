#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>

#include "ps4_dumper.h"
#include "ps5_dumper.h"
#include "utils.h"

#define VERSION "1.07"
#define SANDBOX_PATH "/mnt/sandbox/pfsmnt"

int main(void)
{
    printf_notification("PS5 App Dumper v%s", VERSION);

    /* Wait for USB */
    while (find_usb_and_setup() == -1) {
        printf_notification("Please insert USB (exFAT) into any port...");
        sleep(7);
    }
	
    const char *usb = get_usb_homebrew_path();
    if (!usb) return 1;

    // Config
    int decrypt = read_decrypter_config();
    int elf2fself = read_elf2fself_config();
    int backport = read_backport_config();
    g_enable_logging = read_logging_config();

    char logpath[512];
    snprintf(logpath, sizeof(logpath), "%s/log.txt", usb);
    strncpy(g_log_path, logpath, sizeof(g_log_path)-1);

    write_log(logpath, "=== PS5 App Dumper v%s ===", VERSION);

    /* Detect running app */
    DIR *d = opendir(SANDBOX_PATH);
    if (!d)
    {
        write_log(logpath, "ERROR: Failed to open %s", SANDBOX_PATH);
        printf_notification("Failed to open %s", SANDBOX_PATH);
        return 1;
    }

    char app_folder[128] = {0};
    char patch_folder[128] = {0};
    int is_cusa = 0;

    struct dirent *dp;
    while ((dp = readdir(d))) {
        if (dp->d_type != DT_DIR) continue;
        size_t len = strlen(dp->d_name);
        if (len <= 5) continue;

        int is_ppsa = (strncmp(dp->d_name, "PPSA", 4) == 0);
        is_cusa = (strncmp(dp->d_name, "CUSA", 4) == 0);

        if ((is_ppsa || is_cusa) && strcmp(dp->d_name + len - 5, "-app0") == 0) {
            strncpy(app_folder, dp->d_name, sizeof(app_folder)-1);

            if (is_cusa) {
                char patch_name[128];
                snprintf(patch_name, sizeof(patch_name), "%.*s-patch0", (int)(len - 5), dp->d_name);
                rewinddir(d);
                struct dirent *dp2;
                while ((dp2 = readdir(d)) != NULL) {
                    if (dp2->d_type == DT_DIR && strcmp(dp2->d_name, patch_name) == 0) {
                        strncpy(patch_folder, patch_name, sizeof(patch_folder)-1);
                        break;
                    }
                }
            }
            break;
        }
    }
    closedir(d);

    if (!app_folder[0])
    {
        write_log(logpath, "Please start the App before running the payload...");
        printf_notification("Please start the App before running the payload...");
        return 1;
    }

    write_log(logpath, "Detected App: %s", app_folder);
    printf_notification("Detected: %s", app_folder);
	
    // === CALL DUMPER ===
if (is_cusa) {
    dump_ps4_cusa_app(SANDBOX_PATH, app_folder, patch_folder, usb, decrypt, elf2fself, backport);
} else {
    dump_ps5_ppsa_app(SANDBOX_PATH, app_folder, usb, decrypt, elf2fself, backport);
}

    write_log(logpath, "=== PS5 App Dumper v%s finished ===", VERSION);
    printf_notification("Dump Complete!");
    return 0;
}