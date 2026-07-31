/* Guest ELF import resolution. */

#ifndef __IMPORTS_H__
#define __IMPORTS_H__

#include "so_util.h"

extern DynLibFunction dynlib_functions[];
uintptr_t dynlib_find_export(const char *name);
extern size_t dynlib_numfunctions;

void resolve_module_imports(so_module *mod);

#endif
