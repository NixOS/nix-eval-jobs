#pragma once
///@file

#include <cstddef>
#include <sys/types.h>

/* Current resident memory of a process in MiB, 0 if it is gone. */
auto residentMemoryMiB(pid_t pid) -> size_t;
