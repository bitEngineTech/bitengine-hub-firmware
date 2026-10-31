#pragma once

#include <stddef.h>

/* 见 utf8_trunc.c。单独成文件是为了能在主机上编译测试 —— 与 version_cmp.c
   同样的理由：这类"边界上才出错"的函数在板子上极难归因。 */
void utf8_safe_copy(char *dst, size_t dst_sz, const char *src);
