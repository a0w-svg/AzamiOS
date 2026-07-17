/**
 * sys/mount.h  –  AzamiOS Userspace Device Mounting API
 */
#ifndef _SYS_MOUNT_H
#define _SYS_MOUNT_H

int mount(const char *dev, const char *dir, const char *type);
int unmount(const char *dir);

#endif /* _SYS_MOUNT_H */
