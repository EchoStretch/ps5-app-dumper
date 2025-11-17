#ifndef PS4_DUMPER_H
#define PS4_DUMPER_H

int ps4_dumper_start(const char *title_id,
                      const char *app_folder,
                      const char *patch_folder,
                      const char *usb_path,
                      int do_decrypt,
                      int do_elf2fself,
                      int do_backport);

int dump_ps4_cusa_app(const char *title_id,
                      const char *app_folder,
                      const char *patch_folder,
                      const char *usb_path,
                      int do_decrypt,
                      int do_elf2fself,
                      int do_backport);

#endif