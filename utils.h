#ifndef UTILS_H
#define UTILS_H

#include <stddef.h>

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

#endif