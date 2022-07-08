/*
 * MacOS A-Line Traps taken from online Macintosh Almanac
 *
 * Copyright (c) 2020 Mark Cave-Ayland <mark.cave-ayland@ilande.co.uk>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see
 * <http://www.gnu.org/licenses/lgpl-2.1.html>
 */

#ifndef M68K_ALINE_H
#define M68K_ALINE_H

#include "qemu/osdep.h"
#include "cpu.h"

typedef struct ALine {
    gint opcode;
    const char *name;
    int flags;
} ALine;

void build_alinetrap_hash(M68kCPU *cpu);
const ALine *lookup_alinetrap(GHashTable *g, gint opcode);

#endif
