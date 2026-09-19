#ifndef RESPATH_H
#define RESPATH_H

#include <stddef.h>

void respath_get_base_dir(char *out_dir, size_t out_size);
void respath_resolve(const char *relative, char *out_path, size_t out_size);

#endif
