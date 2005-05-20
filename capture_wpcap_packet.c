/* capture_wpcap_packet.c
 * WinPcap-specific interfaces for low-level information (packet.dll).  
 * We load WinPcap at run
 * time, so that we only need one Ethereal binary and one Tethereal binary
 * for Windows, regardless of whether WinPcap is installed or not.
 *
 * $Id$
 *
 * Ethereal - Network traffic analyzer
 * By Gerald Combs <gerald@ethereal.com>
 * Copyright 2001 Gerald Combs
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#if defined HAVE_LIBPCAP && defined _WIN32

#include <glib.h>
#include <gmodule.h>

/* XXX - yes, I know, I should move cppmagic.h to a generic location. */
#include "tools/lemon/cppmagic.h"

#include <epan/value_string.h>

#include <Packet32.h>
#include <windows.h>
#include <windowsx.h>
#include <Ntddndis.h>

#include "capture_wpcap_packet.h"

gboolean has_wpacket = FALSE;


/* This module will use the PacketRequest function in packet.dll (coming with WinPcap) to "directly" access 
 * the Win32 NDIS network driver(s) and ask for various values (status, statistics, ...).
 *
 * Unfortunately, the definitions required for this are not available through the usual windows header files, 
 * but require the Windows "Device Driver Kit" which is not available for free :-( 
 *
 * Fortunately, the definitions needed to access the various NDIS values are available from various OSS projects:
 * - WinPcap in Ntddndis.h
 * - Ndiswrapper in driver/ndis.h and driver/iw_ndis.h
 * - cygwin (MingW?) in usr/include/w32api/ddk/ndis.h and ntddndis.h
 * - FreeBSD (netperf)
 */

/* The MSDN description of the NDIS driver API is available at:
/* MSDN Home >  MSDN Library >  Win32 and COM Development >  Driver Development Kit >  Network Devices and Protocols >  Reference */
/* "NDIS Objects" */
/* http://msdn.microsoft.com/library/default.asp?url=/library/en-us/network/hh/network/21oidovw_d55042e5-0b8a-4439-8ef2-be7331e98464.xml.asp */

/* Some more interesting links:
 * http://sourceforge.net/projects/ndiswrapper/
 * http://www.osronline.com/lists_archive/windbg/thread521.html
 * http://cvs.sourceforge.net/viewcvs.py/mingw/w32api/include/ddk/ndis.h?view=markup
 * http://cvs.sourceforge.net/viewcvs.py/mingw/w32api/include/ddk/ntddndis.h?view=markup
 */



/******************************************************************************************************************************/
/* stuff to load WinPcap's packet.dll and the functions required from it */

static LPADAPTER (*p_PacketOpenAdapter) (char *adaptername);
static void      (*p_PacketCloseAdapter) (LPADAPTER);
static int       (*p_PacketRequest) (LPADAPTER, int, void *);

typedef struct {
	const char	*name;
	gpointer	*ptr;
	gboolean	optional;
} symbol_table_t;

#define SYM(x, y)	{ STRINGIFY(x) , (gpointer) &CONCAT(p_,x), y }

void
wpcap_packet_load(void)
{

	/* These are the symbols I need or want from packet.dll */
	static const symbol_table_t	symbols[] = {
		SYM(PacketOpenAdapter, FALSE),
		SYM(PacketCloseAdapter, FALSE),
		SYM(PacketRequest, TRUE),
		{ NULL, NULL, FALSE }
	};

	GModule		*wh; /* wpcap handle */
	const symbol_table_t	*sym;

	wh = g_module_open("packet", 0);

	if (!wh) {
		return;
	}

	sym = symbols;
	while (sym->name) {
		if (!g_module_symbol(wh, sym->name, sym->ptr)) {
			if (sym->optional) {
				/*
				 * We don't care if it's missing; we just
				 * don't use it.
				 */
				*sym->ptr = NULL;
			} else {
				/*
				 * We require this symbol.
				 */
				return;
			}
		}
		sym++;
	}


	has_wpacket = TRUE;
}



/******************************************************************************************************************************/
/* functions to access the NDIS driver values */


/* open the interface */
LPADAPTER
wpcap_packet_open(char *if_name)
{
    LPADAPTER adapter;

    adapter = p_PacketOpenAdapter(if_name);

    return adapter;
}


/* close the interface */
void
wpcap_packet_close(LPADAPTER adapter)
{

    p_PacketCloseAdapter(adapter);
}


/* do a packet request call */
int 
wpcap_packet_request(LPADAPTER a, ULONG Oid, int set, char *value, unsigned int *length)
{
    BOOLEAN    Status;
    ULONG      IoCtlBufferLength=(sizeof(PACKET_OID_DATA) + (*length) - 1);
    PPACKET_OID_DATA  OidData;


	g_assert(has_wpacket);

    if(p_PacketRequest == NULL) {
        g_warning("packet_request not available\n");
        return 0;
    }

    OidData=GlobalAllocPtr(GMEM_MOVEABLE | GMEM_ZEROINIT,IoCtlBufferLength);
    if (OidData == NULL) {
        g_warning("packet_link_status failed\n");
        return 0;
    }

    OidData->Oid = Oid;
    OidData->Length = *length;

    Status = p_PacketRequest(a, set, OidData);

    if(Status) {
        g_assert(OidData->Length <= *length);
        memcpy(value, OidData->Data, OidData->Length);
        *length = OidData->Length;
    }

    GlobalFreePtr (OidData);

    if(Status) {
        return 1;
    } else {
        return 0;
    }
}


/* get an UINT value using the packet request call */
int
wpcap_packet_request_uint(LPADAPTER a, ULONG Oid, UINT *value)
{
    BOOLEAN     Status;
    int         length = sizeof(UINT);


    Status = wpcap_packet_request(a, Oid, FALSE /* !set */, (char *) value, &length);
    if(Status) {
        g_assert(length == sizeof(UINT));
        return 1;
    } else {
        return 0;
    }
}


/* get an ULONG value using the NDIS packet request call */
int
wpcap_packet_request_ulong(LPADAPTER a, ULONG Oid, ULONG *value)
{
    BOOLEAN     Status;
    int         length = sizeof(ULONG);


    Status = wpcap_packet_request(a, Oid, FALSE /* !set */, (char *) value, &length);
    if(Status) {
        g_assert(length == sizeof(ULONG));
        return 1;
    } else {
        return 0;
    }
}


#else /* HAVE_LIBPCAP && _WIN32 */

void
wpcap_packet_load(void)
{
	return;
}

#endif /* HAVE_LIBPCAP */
