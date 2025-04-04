/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Support for Intel Camera Imaging ISP subsystem.
 * Copyright (c) 2015, Intel Corporation.
 */

#ifndef __TIMED_CTRL_H_INCLUDED__
#define __TIMED_CTRL_H_INCLUDED__

/*
 * This file is included on every cell {SP,ISP,host} and on every system
 * that uses the input system device(s). It defines the API to DLI bridge
 *
 * System and cell specific interfaces and inline code are included
 * conditionally through Makefile path settings.
 *
 *  - .        system and cell agnostic interfaces, constants and identifiers
 *	- public:  system agnostic, cell specific interfaces
 *	- private: system dependent, cell specific interfaces & inline implementations
 *	- global:  system specific constants and identifiers
 *	- local:   system and cell specific constants and identifiers
 */

#include "system_local.h"
#include "timed_ctrl_local.h"

#ifndef __INLINE_TIMED_CTRL__
#define STORAGE_CLASS_TIMED_CTRL_H extern
#define STORAGE_CLASS_TIMED_CTRL_C
#include "timed_ctrl_public.h"
#else  /* __INLINE_TIMED_CTRL__ */
#define STORAGE_CLASS_TIMED_CTRL_H static inline
#define STORAGE_CLASS_TIMED_CTRL_C static inline
#include "timed_ctrl_private.h"
#endif /* __INLINE_TIMED_CTRL__ */

#endif /* __TIMED_CTRL_H_INCLUDED__ */
