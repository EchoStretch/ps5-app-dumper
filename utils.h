#ifndef UTILS_H
#define UTILS_H

#include <stddef.h>
#include <pthread.h>
#include <time.h>
#include <sys/types.h>

#ifndef ENABLE_LOGGING
#define ENABLE_LOGGING 1
#endif

/* Full PS5 notification struct */
typedef struct {
    int type;                //0x00
    int req_id;              //0x04
    int priority;            //0x08
    int msg_id;              //0x0C
    int target_id;           //0x10
    int user_id;             //0x14
    int unk1;                //0x18
    int unk2;                //0x1C
    int app_id;              //0x20
    int error_num;           //0x24
    int unk3;                //0x28
    char use_icon_image_uri; //0x2C
    char message[1024];      //0x2D
    char uri[1024];          //0x42D
    char unkstr[1024];       //0x82D
} SceNotificationRequest;   //Size = 0xC30

int dir_exists(const char *path);
int file_exists(const char *path);
void mkdirs(const char *path);
int write_log(const char *log_file_path, const char *fmt, ...);
void printf_notification(const char *fmt, ...);
int sceKernelSendNotificationRequest(int device, SceNotificationRequest *req, size_t size, int blocking);

int read_npwr_id(const char *npbind_path, char *npwr_out, size_t out_size);
int fs_copy_file(const char *src, const char *dst);
int copy_file_track(const char *src, const char *dst);
void copy_dir_recursive_tracked(const char *src, const char *dst);
void size_walker(const char *path, size_t *acc);
void *progress_status_func(void *arg);

extern size_t folder_size_current;
extern size_t total_bytes_copied;
extern char current_copied[256];
extern int progress_thread_run;
extern time_t copy_start_time;

int sceKernelAioSubmitReadCommands(void *reqs, uint32_t n, uint32_t prio, int *id);
int sceKernelAioSubmitWriteCommands(void *reqs, uint32_t n, uint32_t prio, int *id);
int sceKernelAioWaitRequest(int req_id, int *state, uint32_t *usec);
int sceKernelAioDeleteRequest(int req_id, int *ret);

int  find_usb_and_setup(void);
int  read_decrypter_config(void);
const char* get_usb_homebrew_path(void);

#endif /* UTILS_H */