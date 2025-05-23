/* Generic memory read/write for hardware simulator models.
   Copyright (C) 1997-2025 Free Software Foundation, Inc.
   Contributed by Cygnus Support.

This file is part of GDB, the GNU debugger.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

/* This must come before any other includes.  */
#include "defs.h"

#include "sim-main.h"
#include "sim-assert.h"

/* Generic implementation of sim_read that works with simulators
   modeling real hardware */

uint64_t
sim_read (SIM_DESC sd, uint64_t addr, void *buffer, uint64_t length)
{
  SIM_ASSERT (STATE_MAGIC (sd) == SIM_MAGIC_NUMBER);
  return sim_core_read_buffer (sd, NULL, read_map,
			       buffer, addr, length);
}

uint64_t
sim_write (SIM_DESC sd, uint64_t addr, const void *buffer, uint64_t length)
{
  SIM_ASSERT (STATE_MAGIC (sd) == SIM_MAGIC_NUMBER);
  return sim_core_write_buffer (sd, NULL, write_map,
				buffer, addr, length);
}
