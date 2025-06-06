/* Copyright (C) 2024 MariaDB Corporation
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

#define DDLPKG_DLLEXPORT
#include "ddlpkg.h"
#undef DDLPKG_DLLEXPORT

using namespace std;

namespace ddlpackage
{
DebugDDLStatement::DebugDDLStatement(uint32_t debugLevel)
: fDebugLevel(debugLevel)
{
}

DebugDDLStatement::DebugDDLStatement()
: fDebugLevel(0)
{
}

ostream& DebugDDLStatement::put(ostream& os) const
{
  os << "DDL debug level: " << fDebugLevel << endl;
  return os;
}
}  // namespace ddlpackage
