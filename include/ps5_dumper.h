#pragma once

int dump_ps5_ppsa_app(
    const char *sandbox,
    const char *app_folder,
    const char *usb_path,
    int do_decrypt,
    int do_elf2fself,
    int do_backport
);