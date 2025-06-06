/* Copyright (C) 2014 InfiniDB, Inc.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; version 2 of
   the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
   MA 02110-1301, USA. */

/***********************************************************************
 *   $Id: droppartitionprocessor.h 6566 2010-04-27 18:02:51Z rdempsey $
 *
 *
 ***********************************************************************/
/** @file */
#pragma once

#include "ddlpackageprocessor.h"

#define EXPORT

namespace ddlpackageprocessor
{
/** @brief specialization of a DDLPacakageProcessor
 * for interacting with the Write Engine to process
 * drop table ddl statements.
 */
class DropPartitionProcessor : public DDLPackageProcessor, FormatStatementString
{
 public:
  DropPartitionProcessor(BRM::DBRM* aDbrm) : DDLPackageProcessor(aDbrm)
  {
  }

 protected:
 private:
  /** @brief process a drop table statement
   *
   *  @param dropTableStmt the drop table statement
   */
  DDLResult processPackageInternal(ddlpackage::SqlStatement* dropPartitionStmt);
};
}  // namespace ddlpackageprocessor

#undef EXPORT
