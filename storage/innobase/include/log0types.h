/*****************************************************************************

Copyright (c) 2013, Oracle and/or its affiliates. All rights reserved.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file include/log0types.h
Log types

Created 2013-03-15 Sunny Bains
*******************************************************/

#ifndef log0types_h
#define log0types_h

#include "univ.i"

/* Type used for all log sequence number storage and arithmetics */
typedef	ib_uint64_t		lsn_t;

#define LSN_MAX			IB_UINT64_MAX

#define LSN_PF			UINT64PF

#endif /* log0types_h */
