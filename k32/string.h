/* String / memory helpers */
#ifndef ROS_STRING_H
#define ROS_STRING_H

#include "types.h"

void *memset(void *dst, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int   memcmp(const void *a, const void *b, size_t n);

size_t strlen(const char *s);
char  *strcpy(char *dst, const char *src);
char  *strncpy(char *dst, const char *src, size_t n);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
int    strcasecmp(const char *a, const char *b);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);

int    atoi(const char *s);
void   utoa(u32 v, char *buf);      /* decimal */
void   utox(u32 v, char *buf);      /* 8 lowercase hex digits */

#endif
