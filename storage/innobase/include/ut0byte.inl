/*****************************************************************************

Copyright (c) 1994, 2013, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2019, 2020, MariaDB Corporation.

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

/**************************************************************//**
@file include/ut0byte.ic
Utilities for byte operations

Created 5/30/1994 Heikki Tuuri
*******************************************************************/

/*******************************************************//**
Creates a 64-bit integer out of two 32-bit integers.
@return created integer */
UNIV_INLINE
ib_uint64_t
ut_ull_create(
/*==========*/
	ulint	high,	/*!< in: high-order 32 bits */
	ulint	low)	/*!< in: low-order 32 bits */
{
	ut_ad(high <= ULINT32_MASK);
	ut_ad(low <= ULINT32_MASK);
	return(((ib_uint64_t) high) << 32 | low);
}

/********************************************************//**
Rounds a 64-bit integer downward to a multiple of a power of 2.
@return rounded value */
UNIV_INLINE
ib_uint64_t
ut_uint64_align_down(
/*=================*/
	ib_uint64_t	 n,		/*!< in: number to be rounded */
	ulint		 align_no)	/*!< in: align by this number
					which must be a power of 2 */
{
	ut_ad(align_no > 0);
	ut_ad(ut_is_2pow(align_no));

	return(n & ~((ib_uint64_t) align_no - 1));
}

/********************************************************//**
Rounds ib_uint64_t upward to a multiple of a power of 2.
@return rounded value */
UNIV_INLINE
ib_uint64_t
ut_uint64_align_up(
/*===============*/
	ib_uint64_t	 n,		/*!< in: number to be rounded */
	ulint		 align_no)	/*!< in: align by this number
					which must be a power of 2 */
{
	ib_uint64_t	align_1 = (ib_uint64_t) align_no - 1;

	ut_ad(align_no > 0);
	ut_ad(ut_is_2pow(align_no));

	return((n + align_1) & ~align_1);
}

/*****************************************************************//**
Gets the nth bit of a ulint.
@return TRUE if nth bit is 1; 0th bit is defined to be the least significant */
UNIV_INLINE
ibool
ut_bit_get_nth(
/*===========*/
	ulint	a,	/*!< in: ulint */
	ulint	n)	/*!< in: nth bit requested */
{
	ut_ad(n < 8 * sizeof(ulint));
	return(1 & (a >> n));
}
