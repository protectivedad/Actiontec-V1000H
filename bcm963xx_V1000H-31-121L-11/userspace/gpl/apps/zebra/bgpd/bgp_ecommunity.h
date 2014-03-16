/* BGP Extended Communities Attribute.
   Copyright (C) 2000 Kunihiro Ishiguro <kunihiro@zebra.org>

This file is part of GNU Zebra.

GNU Zebra is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation; either version 2, or (at your option) any
later version.

GNU Zebra is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
General Public License for more details.

You should have received a copy of the GNU General Public License
along with GNU Zebra; see the file COPYING.  If not, write to the Free
Software Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
02111-1307, USA.  */

/* High-order octet of the Extended Communities type field.  */
#define ECOMMUNITY_ENCODE_AS       0x00
#define ECOMMUNITY_ENCODE_IP       0x01

/* Low-order octet of the Extended Communityes type field.  */
#define ECOMMUNITY_ROUTE_TARGET    0x02
#define ECOMMUNITY_SITE_ORIGIN     0x03

/* Extended communities attribute string format.  */
#define ECOMMUNITY_FORMAT_ROUTE_MAP            0
#define ECOMMUNITY_FORMAT_COMMUNITY_LIST       1
#define ECOMMUNITY_FORMAT_DISPLAY              2

/* BGP routing distinguisher size.  */
#ifndef BGP_RD_SIZE
#define BGP_RD_SIZE                8
#endif /* BGP_RD_SIZE */

/* Extended Communities attribute.  */
struct ecommunity
{
  unsigned long refcnt;
  int size;
  u_char *val;
  char *str;
};

#define ecom_length(X)    ((X)->size * 8)

void ecommunity_init (void);
void ecommunity_free (struct ecommunity *);
struct ecommunity *ecommunity_new (void);
struct ecommunity *ecommunity_parse (char *, u_short);
struct ecommunity *ecommunity_dup (struct ecommunity *);
struct ecommunity *ecommunity_merge (struct ecommunity *, struct ecommunity *);
struct ecommunity *ecommunity_intern (struct ecommunity *);
int ecommunity_cmp (struct ecommunity *, struct ecommunity *);
void ecommunity_unintern (struct ecommunity *);
unsigned int ecommunity_hash_make (struct ecommunity *);

void ecommunity_rd2com (struct bgp_rd *, u_char);
struct ecommunity *ecommunity_str2com (char *, int, int);
void ecommunity_vty_out (struct vty *, struct ecommunity *);
char *ecommunity_ecom2str (struct ecommunity *, int);
