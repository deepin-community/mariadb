/* Copyright (c) 2000, 2014, Oracle and/or its affiliates.
   Copyright (c) 2009, 2021, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#include "strings_def.h"
#include <m_ctype.h>
#include "ctype-mb.h"

#ifdef USE_MB


static inline const MY_CASEFOLD_CHARACTER*
get_case_info_for_ch(CHARSET_INFO *cs, uint page, uint offs)
{
  const MY_CASEFOLD_CHARACTER *p;
  return cs->casefold && (p= cs->casefold->page[page]) ? &p[offs] : NULL;
}


/*
  Case folding functions for CJK character set.
  Case conversion can optionally reduce string octet length.
  For example, in EUCKR,
    _euckr 0xA9A5 == "LATIN LETTER DOTLESS I" (Turkish letter)
  is upper-cased to to
    _euckr 0x49 "LATIN CAPITAL LETTER I"  ('usual' letter I)
  Length is reduced in this example from two bytes to one byte.
*/
static size_t
my_casefold_mb(CHARSET_INFO *cs,
               const char *src, size_t srclen,
               char *dst, size_t dstlen __attribute__((unused)),
               const uchar *map,
               size_t is_upper)
{
  const char *srcend= src + srclen;
  char *dst0= dst;

  DBUG_ASSERT(cs->mbmaxlen == 2);

  while (src < srcend)
  {
    size_t mblen= my_ismbchar(cs, src, srcend);
    if (mblen)
    {
      const MY_CASEFOLD_CHARACTER *ch;
      if ((ch= get_case_info_for_ch(cs, (uchar) src[0], (uchar) src[1])))
      {
        int code= is_upper ? ch->toupper : ch->tolower;
        src+= 2;
        if (code > 0xFF)
          *dst++= code >> 8;
        *dst++= code & 0xFF;
      }
      else
      {
        *dst++= *src++;
        *dst++= *src++;
      }
    }
    else
    {
      *dst++= (char) map[(uchar) *src++];
    }
  }
  return (size_t) (dst - dst0);
}


size_t
my_casedn_mb(CHARSET_INFO * cs, const char *src, size_t srclen,
                    char *dst, size_t dstlen)
{
  DBUG_ASSERT(src != NULL); /* Avoid UBSAN nullptr-with-offset */
  DBUG_ASSERT(dstlen >= srclen * cs->cset->casedn_multiply(cs));
  DBUG_ASSERT(src != dst || cs->cset->casedn_multiply(cs) == 1);
  return my_casefold_mb(cs, src, srclen, dst, dstlen, cs->to_lower, 0);
}


size_t
my_caseup_mb(CHARSET_INFO * cs, const char *src, size_t srclen,
             char *dst, size_t dstlen)
{
  DBUG_ASSERT(src != NULL); /* Avoid UBSAN nullptr-with-offset */
  DBUG_ASSERT(dstlen >= srclen * cs->cset->caseup_multiply(cs));
  DBUG_ASSERT(src != dst || cs->cset->caseup_multiply(cs) == 1);
  return my_casefold_mb(cs, src, srclen, dst, dstlen, cs->to_upper, 1);
}


/*
** Compare string against string with wildcard
**	0 if matched
**	-1 if not matched with wildcard
**	 1 if matched with wildcard
*/

#define INC_PTR(cs,A,B) A+=(my_ismbchar(cs,A,B) ? my_ismbchar(cs,A,B) : 1)

#define likeconv(s,A) (uchar) (s)->sort_order[(uchar) (A)]

static
int my_wildcmp_mb_impl(CHARSET_INFO *cs,
                       const char *str,const char *str_end,
                       const char *wildstr,const char *wildend,
                       int escape, int w_one, int w_many, int recurse_level)
{
  int result= -1;				/* Not found, using wildcards */

  if (my_string_stack_guard && my_string_stack_guard(recurse_level))
    return 1;
  while (wildstr != wildend)
  {
    while (*wildstr != w_many && *wildstr != w_one)
    {
      int l;
      if (*wildstr == escape && wildstr+1 != wildend)
	wildstr++;
      if ((l = my_ismbchar(cs, wildstr, wildend)))
      {
	  if (str+l > str_end || memcmp(str, wildstr, l) != 0)
	      return 1;
	  str += l;
	  wildstr += l;
      }
      else
      if (str == str_end || likeconv(cs,*wildstr++) != likeconv(cs,*str++))
	return(1);				/* No match */
      if (wildstr == wildend)
	return (str != str_end);		/* Match if both are at end */
      result=1;					/* Found an anchor char */
    }
    if (*wildstr == w_one)
    {
      do
      {
	if (str == str_end)			/* Skip one char if possible */
	  return (result);
	INC_PTR(cs,str,str_end);
      } while (++wildstr < wildend && *wildstr == w_one);
      if (wildstr == wildend)
	break;
    }
    if (*wildstr == w_many)
    {						/* Found w_many */
      uchar cmp;
      const char* mb = wildstr;
      int mb_len=0;
      
      wildstr++;
      /* Remove any '%' and '_' from the wild search string */
      for (; wildstr != wildend ; wildstr++)
      {
	if (*wildstr == w_many)
	  continue;
	if (*wildstr == w_one)
	{
	  if (str == str_end)
	    return (-1);
	  INC_PTR(cs,str,str_end);
	  continue;
	}
	break;					/* Not a wild character */
      }
      if (wildstr == wildend)
	return(0);				/* Ok if w_many is last */
      if (str == str_end)
	return -1;
      
      if ((cmp= *wildstr) == escape && wildstr+1 != wildend)
	cmp= *++wildstr;
	
      mb=wildstr;
      mb_len= my_ismbchar(cs, wildstr, wildend);
      INC_PTR(cs,wildstr,wildend);		/* This is compared trough cmp */
      cmp=likeconv(cs,cmp);   
      do
      {
        for (;;)
        {
          if (str >= str_end)
            return -1;
          if (mb_len)
          {
            if (str+mb_len <= str_end && memcmp(str, mb, mb_len) == 0)
            {
              str += mb_len;
              break;
            }
          }
          else if (!my_ismbchar(cs, str, str_end) &&
                   likeconv(cs,*str) == cmp)
          {
            str++;
            break;
          }
          INC_PTR(cs,str, str_end);
        }
	{
	  int tmp=my_wildcmp_mb_impl(cs,str,str_end,wildstr,wildend,escape,w_one,
                                     w_many, recurse_level + 1);
	  if (tmp <= 0)
	    return (tmp);
	}
      } while (str != str_end);
      return(-1);
    }
  }
  return (str != str_end ? 1 : 0);
}

int my_wildcmp_mb(CHARSET_INFO *cs,
                  const char *str,const char *str_end,
                  const char *wildstr,const char *wildend,
                  int escape, int w_one, int w_many)
{
  return my_wildcmp_mb_impl(cs, str, str_end,
                            wildstr, wildend,
                            escape, w_one, w_many, 1);
}


size_t my_numchars_mb(CHARSET_INFO *cs __attribute__((unused)),
		      const char *pos, const char *end)
{
  register size_t count= 0;
  while (pos < end) 
  {
    uint mb_len;
    pos+= (mb_len= my_ismbchar(cs,pos,end)) ? mb_len : 1;
    count++;
  }
  return count;
}


size_t my_charpos_mb(CHARSET_INFO *cs __attribute__((unused)),
		     const char *pos, const char *end, size_t length)
{
  const char *start= pos;
  
  while (length && pos < end)
  {
    uint mb_len;
    pos+= (mb_len= my_ismbchar(cs, pos, end)) ? mb_len : 1;
    length--;
  }
  return (size_t) (length ? end+2-start : pos-start);
}


/*
  Append a badly formed piece of string.
  Bad bytes are fixed to '?'.
  
  @param to        The destination string
  @param to_end    The end of the destination string
  @param from      The source string
  @param from_end  The end of the source string
  @param nchars    Write not more than "nchars" characters.
  @param status    Copying status, must be previously initialized,
                   e.g. using well_formed_char_length() on the original
                   full source string.
*/
static size_t
my_append_fix_badly_formed_tail(CHARSET_INFO *cs,
                                char *to, char *to_end,
                                const char *from, const char *from_end,
                                size_t nchars,
                                MY_STRCOPY_STATUS *status)
{
  char *to0= to;

  for ( ; nchars; nchars--)
  {
    int chlen;
    if ((chlen= my_ci_charlen(cs, (const uchar*) from,
                                  (const uchar *) from_end)) > 0)
    {
      /* Found a valid character */         /* chlen == 1..MBMAXLEN  */
      DBUG_ASSERT(chlen <= (int) cs->mbmaxlen);
      if (to + chlen > to_end)
        goto end;                           /* Does not fit to "to" */
      memcpy(to, from, (size_t) chlen);
      from+= chlen;
      to+= chlen;
      continue;
    }
    if (chlen == MY_CS_ILSEQ)              /* chlen == 0 */
    {
      DBUG_ASSERT(from < from_end);  /* Shouldn't get MY_CS_ILSEQ if empty */
      goto bad;
    }
    /* Got an incomplete character */       /* chlen == MY_CS_TOOSMALLXXX  */
    DBUG_ASSERT(chlen >= MY_CS_TOOSMALL6); 
    DBUG_ASSERT(chlen <= MY_CS_TOOSMALL);
    if (from >= from_end)                   
      break;                                /* End of the source string    */
bad:
    /* Bad byte sequence, or incomplete character found */
    if (!status->m_well_formed_error_pos)
      status->m_well_formed_error_pos= from;

    if ((chlen= my_ci_wc_mb(cs, '?', (uchar*) to, (uchar *) to_end)) <= 0)
      break; /* Question mark does not fit into the destination */
    to+= chlen;
    from++;
  }
end:
  status->m_source_end_pos= from;
  return to - to0;
}


size_t
my_copy_fix_mb(CHARSET_INFO *cs,
               char *dst, size_t dst_length,
               const char *src, size_t src_length,
               size_t nchars, MY_STRCOPY_STATUS *status)
{
  size_t well_formed_nchars;
  size_t well_formed_length;
  size_t fixed_length;
  size_t min_length= MY_MIN(src_length, dst_length);

  well_formed_nchars= my_ci_well_formed_char_length(cs, src, src + min_length,
                                                        nchars, status);
  DBUG_ASSERT(well_formed_nchars <= nchars);
  well_formed_length= status->m_source_end_pos - src;
  if (well_formed_length)
    memmove(dst, src, well_formed_length);
  if (!status->m_well_formed_error_pos)
    return well_formed_length;

  fixed_length= my_append_fix_badly_formed_tail(cs,
                                                dst + well_formed_length,
                                                dst + dst_length,
                                                src + well_formed_length,
                                                src + src_length,
                                                nchars - well_formed_nchars,
                                                status);
  return well_formed_length + fixed_length;
}


uint my_instr_mb(CHARSET_INFO *cs,
                 const char *b, size_t b_length, 
                 const char *s, size_t s_length,
                 my_match_t *match, uint nmatch)
{
  register const char *end, *b0;
  int res= 0;
  
  if (s_length <= b_length)
  {
    if (!s_length)
    {
      if (nmatch)
      {
        match->beg= 0;
        match->end= 0;
        match->mb_len= 0;
      }
      return 1;		/* Empty string is always found */
    }
    
    b0= b;
    end= b+b_length-s_length+1;
    
    while (b < end)
    {
      int mb_len;
      
      if (!my_ci_strnncoll(cs, (const uchar *) b, s_length,
                               (const uchar *) s, s_length, 0))
      {
        if (nmatch)
        {
          match[0].beg= 0;
          match[0].end= (uint) (b-b0);
          match[0].mb_len= res;
          if (nmatch > 1)
          {
            match[1].beg= match[0].end;
            match[1].end= (uint)(match[0].end+s_length);
            match[1].mb_len= 0;	/* Not computed */
          }
        }
        return 2;
      }
      mb_len= (mb_len= my_ismbchar(cs, b, end)) ? mb_len : 1;
      b+= mb_len;
      b_length-= mb_len;
      res++;
    }
  }
  return 0;
}


/*
  Copy one non-ascii character.
  "dst" must have enough room for the character.
  Note, we don't use sort_order[] in this macros.
  This is correct even for case insensitive collations:
  - basic Latin letters are processed outside this macros;
  - for other characters sort_order[x] is equal to x.
*/
#define my_strnxfrm_mb_non_ascii_char(cs, dst, src, se)                  \
{                                                                        \
  switch (my_ismbchar(cs, (const char *) src, (const char *) se)) {      \
  case 4:                                                                \
    *dst++= *src++;                                                      \
    /* fall through */                                                   \
  case 3:                                                                \
    *dst++= *src++;                                                      \
    /* fall through */                                                   \
  case 2:                                                                \
    *dst++= *src++;                                                      \
    /* fall through */                                                   \
  case 0:                                                                \
    *dst++= *src++; /* byte in range 0x80..0xFF which is not MB head */  \
  }                                                                      \
}


/*
  For character sets with two or three byte multi-byte
  characters having multibyte weights *equal* to their codes:
  cp932, euckr, gb2312, sjis, eucjpms, ujis.
*/
my_strnxfrm_ret_t my_strnxfrm_mb_internal(CHARSET_INFO *cs,
                                          uchar *dst, uchar *de,
                                          uint *nweights,
                                          const uchar *src, size_t srclen)
{
  uchar *d0= dst;
  const uchar *src0= src;
  const uchar *se= src + srclen;
  const uchar *sort_order= cs->sort_order;
  uint warnings= 0;

  DBUG_ASSERT(cs->mbmaxlen <= 4);

  /*
    If "srclen" is smaller than both "dstlen" and "nweights"
    then we can run a simplified loop -
    without checking "nweights" and "de".
  */
  if (de >= d0 + srclen && *nweights >= srclen)
  {
    if (sort_order)
    {
      /* Optimized version for a case insensitive collation */
      for (; src < se; (*nweights)--)
      {
        if (*src < 128) /* quickly catch ASCII characters */
          *dst++= sort_order[*src++];
        else
          my_strnxfrm_mb_non_ascii_char(cs, dst, src, se);
      }
    }
    else
    {
      /* Optimized version for a case sensitive collation (no sort_order) */
      for (; src < se; (*nweights)--)
      {
        if (*src < 128) /* quickly catch ASCII characters */
          *dst++= *src++;
        else
          my_strnxfrm_mb_non_ascii_char(cs, dst, src, se);
      }
    }
    return my_strnxfrm_ret_construct(dst - d0, src - src0, 0);
  }

  /*
    A thourough loop, checking all possible limits:
    "se", "nweights" and "de".
  */
  for (; src < se && *nweights && dst < de; (*nweights)--)
  {
    int chlen;
    if (*src < 128 || !(chlen= my_ismbchar(cs, (const char *) src,
                                               (const char *) se)))
    {
      /* Single byte character */
      *dst++= sort_order ? sort_order[*src++] : *src++;
    }
    else
    {
      /* Multi-byte character */
      size_t len= (dst + chlen <= de) ? chlen : de - dst;
      if (dst + chlen > de)
        warnings|= MY_STRNXFRM_TRUNCATED_WEIGHT_REAL_CHAR;
      memcpy(dst, src, len);
      dst+= len;
      src+= chlen;
    }
  }

  return my_strnxfrm_ret_construct(dst - d0, src - src0,
           warnings |
           (src < se ? MY_STRNXFRM_TRUNCATED_WEIGHT_REAL_CHAR : 0));
}


my_strnxfrm_ret_t
my_strnxfrm_mb(CHARSET_INFO *cs,
               uchar *dst, size_t dstlen, uint nweights,
               const uchar *src, size_t srclen, uint flags)
{
  uchar *de= dst + dstlen;
  my_strnxfrm_ret_t rc= my_strnxfrm_mb_internal(cs, dst, de, &nweights,
                                                src, srclen);
  my_strnxfrm_ret_t rcpad= my_strxfrm_pad_desc_and_reverse(cs, dst,
                                                      dst + rc.m_result_length,
                                                      de, nweights, flags, 0);

  return my_strnxfrm_ret_construct(rcpad.m_result_length,
                                   rc.m_source_length_used,
                                   rc.m_warnings | rcpad.m_warnings);
}


my_strnxfrm_ret_t
my_strnxfrm_mb_nopad(CHARSET_INFO *cs,
                     uchar *dst, size_t dstlen, uint nweights,
                     const uchar *src, size_t srclen, uint flags)
{
  uchar *de= dst + dstlen;
  my_strnxfrm_ret_t rc= my_strnxfrm_mb_internal(cs, dst, de, &nweights,
                                                src, srclen);
  my_strnxfrm_ret_t rcpad= my_strxfrm_pad_desc_and_reverse_nopad(cs, dst,
                                                     dst + rc.m_result_length,
                                                     de, nweights, flags, 0);
  return my_strnxfrm_ret_construct(rcpad.m_result_length,
                                   rc.m_source_length_used,
                                   rc.m_warnings | rcpad.m_warnings);;
}


void
my_hash_sort_mb_nopad_bin(CHARSET_INFO *cs __attribute__((unused)),
                          const uchar *key, size_t len,ulong *nr1, ulong *nr2)
{
  register ulong m1= *nr1, m2= *nr2;
  const uchar *end= key + len;
  for (; key < end ; key++)
  {
    MY_HASH_ADD(m1, m2, (uint)*key);
  }
  *nr1= m1;
  *nr2= m2;
}


void
my_hash_sort_mb_bin(CHARSET_INFO *cs __attribute__((unused)),
                    const uchar *key, size_t len,ulong *nr1, ulong *nr2)
{
  /*
     Remove trailing spaces. We have to do this to be able to compare
    'A ' and 'A' as identical
  */
  const uchar *end= skip_trailing_space(key, len);
  my_hash_sort_mb_nopad_bin(cs, key, end - key, nr1, nr2);
}


static inline size_t
my_repeat_char_native(CHARSET_INFO *cs,
                      uchar *dst, size_t dst_size, size_t nchars,
                      my_wc_t native_code)
{
  uchar *dst0= dst;
  uchar *dstend= dst + dst_size;
  int chlen= my_ci_native_to_mb(cs, native_code, dst, dstend);
  if (chlen < 1 /* Not enough space */ || !nchars)
    return 0;
  for (dst+= chlen, nchars--;
       dst + chlen <= dstend && nchars > 0;
       dst+= chlen, nchars--)
    memcpy(dst, dst0, chlen);
  return dst - dst0;
}


size_t my_min_str_mb_simple(CHARSET_INFO *cs,
                            uchar *dst, size_t dst_size, size_t nchars)
{
  return my_repeat_char_native(cs, dst, dst_size, nchars, cs->min_sort_char);
}


size_t my_min_str_mb_simple_nopad(CHARSET_INFO *cs,
                                  uchar *dst, size_t dst_size, size_t nchars)
{
  /* For NOPAD collations, the empty string is the smallest possible */
  return 0;
}


size_t my_max_str_mb_simple(CHARSET_INFO *cs,
                            uchar *dst, size_t dst_size, size_t nchars)
{
  return my_repeat_char_native(cs, dst, dst_size, nchars, cs->max_sort_char);
}


/* 
  Fill the given buffer with 'maximum character' for given charset
  SYNOPSIS
      pad_max_char()
      cs   Character set
      str  Start of buffer to fill
      end  End of buffer to fill

  DESCRIPTION
      Write max key:
      - for non-Unicode character sets:
        just bfill using max_sort_char if max_sort_char is one byte.
        In case when max_sort_char is two bytes, fill with double-byte pairs
        and optionally pad with a single space character.
      - for Unicode character set (utf-8):
        create a buffer with multibyte representation of the max_sort_char
        character, and copy it into max_str in a loop. 
*/
static void pad_max_char(CHARSET_INFO *cs, char *str, char *end)
{
  char buf[10];
  char buflen= my_ci_native_to_mb(cs, cs->max_sort_char, (uchar*) buf,
                                      (uchar*) buf + sizeof(buf));
  DBUG_ASSERT(buflen > 0);
  do
  {
    if ((str + buflen) <= end)
    {
      /* Enough space for the character */
      memcpy(str, buf, buflen);
      str+= buflen;
    }
    else
    {
      /* 
        There is no space for whole multibyte
        character, then add trailing spaces.
      */  
      *str++= ' ';
    }
  } while (str < end);
}

/*
** Calculate min_str and max_str that ranges a LIKE string.
** Arguments:
** ptr		Pointer to LIKE string.
** ptr_length	Length of LIKE string.
** escape	Escape character in LIKE.  (Normally '\').
**		All escape characters should be removed from min_str and max_str
** res_length	Length of min_str and max_str.
** min_str	Smallest case sensitive string that ranges LIKE.
**		Should be space padded to res_length.
** max_str	Largest case sensitive string that ranges LIKE.
**		Normally padded with the biggest character sort value.
**
** The function should return 0 if ok and 1 if the LIKE string can't be
** optimized !
*/

my_bool my_like_range_mb(CHARSET_INFO *cs,
			 const char *ptr,size_t ptr_length,
			 pbool escape, pbool w_one, pbool w_many,
			 size_t res_length,
			 char *min_str,char *max_str,
			 size_t *min_length,size_t *max_length)
{
  uint mb_len;
  const char *end= ptr + ptr_length;
  char *min_org= min_str;
  char *min_end= min_str + res_length;
  char *max_end= max_str + res_length;
  size_t maxcharlen= res_length / cs->mbmaxlen;
  const MY_CONTRACTIONS *contractions= my_charset_get_contractions(cs, 0);

  for (; ptr != end && min_str != min_end && maxcharlen ; maxcharlen--)
  {
    /* We assume here that escape, w_any, w_namy are one-byte characters */
    if (*ptr == escape && ptr+1 != end)
      ptr++;                                    /* Skip escape */
    else if (*ptr == w_one || *ptr == w_many)   /* '_' and '%' in SQL */
    {      
fill_max_and_min:
      /*
        Calculate length of keys:
        'a\0\0... is the smallest possible string when we have space expand
        a\ff\ff... is the biggest possible string
      */
      *min_length= (cs->state & (MY_CS_BINSORT | MY_CS_NOPAD)) ?
                    (size_t) (min_str - min_org) :
                    res_length;
      /* Create min key  */
      do
      {
	*min_str++= (char) cs->min_sort_char;
      } while (min_str != min_end);
      
      /* 
        Write max key: create a buffer with multibyte
        representation of the max_sort_char character,
        and copy it into max_str in a loop. 
      */
      *max_length= res_length;
      pad_max_char(cs, max_str, max_end);
      return 0;
    }
    if ((mb_len= my_ismbchar(cs, ptr, end)) > 1)
    {
      if (ptr+mb_len > end || min_str+mb_len > min_end)
        break;
      while (mb_len--)
       *min_str++= *max_str++= *ptr++;
    }
    else
    {
      /*
        Special case for collations with contractions.
        For example, in Chezh, 'ch' is a separate letter
        which is sorted between 'h' and 'i'.
        If the pattern 'abc%', 'c' at the end can mean:
        - letter 'c' itself,
        - beginning of the contraction 'ch'.

        If we simply return this LIKE range:

         'abc\min\min\min' and 'abc\max\max\max'

        then this query: SELECT * FROM t1 WHERE a LIKE 'abc%'
        will only find values starting from 'abc[^h]',
        but won't find values starting from 'abch'.

        We must ignore contraction heads followed by w_one or w_many.
        ('Contraction head' means any letter which can be the first
        letter in a contraction)

        For example, for Czech 'abc%', we will return LIKE range,
        which is equal to LIKE range for 'ab%':

        'ab\min\min\min\min' and 'ab\max\max\max\max'.

      */
      if (contractions && ptr + 1 < end &&
          my_uca_can_be_contraction_head(contractions, (uchar) *ptr))
      {
        /* Ptr[0] is a contraction head. */
        
        if (ptr[1] == w_one || ptr[1] == w_many)
        {
          /* Contraction head followed by a wildcard, quit. */
          goto fill_max_and_min;
        }
        
        /*
          Some letters can be both contraction heads and contraction tails.
          For example, in Danish 'aa' is a separate single letter which
          is sorted after 'z'. So 'a' can be both head and tail.
          
          If ptr[0]+ptr[1] is a contraction,
          then put both letters together.
          
          If ptr[1] can be a contraction part, but ptr[0]+ptr[1]
          is not a contraction, then we put only ptr[0],
          and continue with ptr[1] on the next loop.
        */
        if (my_uca_can_be_contraction_tail(contractions, (uchar) ptr[1]) &&
            my_uca_contraction2_weight(contractions, (uchar) ptr[0], ptr[1]))
        {
          /* Contraction found */
          if (maxcharlen == 1 || min_str + 1 >= min_end)
          {
            /* Both contraction parts don't fit, quit */
            goto fill_max_and_min;
          }
          
          /* Put contraction head */
          *min_str++= *max_str++= *ptr++;
          maxcharlen--;
        }
      }
      /* Put contraction tail, or a single character */
      *min_str++= *max_str++= *ptr++;    
    }
  }

  *min_length= *max_length = (size_t) (min_str - min_org);
  while (min_str != min_end)
    *min_str++= *max_str++= ' ';           /* Because if key compression */
  return 0;
}


/**
   Calculate min_str and max_str that ranges a LIKE string.
   Generic function, currently used for ucs2, utf16, utf32,
   but should be suitable for any other character sets with
   cs->min_sort_char and cs->max_sort_char represented in
   Unicode code points.

   @param cs           Character set and collation pointer
   @param ptr          Pointer to LIKE pattern.
   @param ptr_length   Length of LIKE pattern.
   @param escape       Escape character pattern,  typically '\'.
   @param w_one        'One character' pattern,   typically '_'.
   @param w_many       'Many characters' pattern, typically '%'.
   @param res_length   Length of min_str and max_str.

   @param[out] min_str Smallest string that ranges LIKE.
   @param[out] max_str Largest string that ranges LIKE.
   @param[out] min_len Length of min_str
   @param[out] max_len Length of max_str

   @return Optimization status.
   @retval FALSE if LIKE pattern can be optimized
   @rerval TRUE if LIKE can't be optimized.
*/
my_bool
my_like_range_generic(CHARSET_INFO *cs,
                      const char *ptr, size_t ptr_length,
                      pbool escape, pbool w_one, pbool w_many,
                      size_t res_length,
                      char *min_str,char *max_str,
                      size_t *min_length,size_t *max_length)
{
  const char *end= ptr + ptr_length;
  const char *min_org= min_str;
  const char *max_org= max_str;
  char *min_end= min_str + res_length;
  char *max_end= max_str + res_length;
  size_t charlen= res_length / cs->mbmaxlen;
  size_t res_length_diff;
  const MY_CONTRACTIONS *contractions= my_charset_get_contractions(cs, 0);

  for ( ; charlen > 0; charlen--)
  {
    my_wc_t wc, wc2;
    int res;
    if ((res= my_ci_mb_wc(cs, &wc, (uchar*) ptr, (uchar*) end)) <= 0)
    {
      if (res == MY_CS_ILSEQ) /* Bad sequence */
        return TRUE; /* min_length and max_length are not important */
      break; /* End of the string */
    }
    ptr+= res;

    if (wc == (my_wc_t) escape)
    {
      if ((res= my_ci_mb_wc(cs, &wc, (uchar*) ptr, (uchar*) end)) <= 0)
      {
        if (res == MY_CS_ILSEQ)
          return TRUE; /* min_length and max_length are not important */
        /*
           End of the string: Escape is the last character.
           Put escape as a normal character.
           We'll will leave the loop on the next iteration.
        */
      }
      else
        ptr+= res;

      /* Put escape character to min_str and max_str  */
      if ((res= my_ci_wc_mb(cs, wc, (uchar*) min_str, (uchar*) min_end)) <= 0)
        goto pad_set_lengths; /* No space */
      min_str+= res;

      if ((res= my_ci_wc_mb(cs, wc, (uchar*) max_str, (uchar*) max_end)) <= 0)
        goto pad_set_lengths; /* No space */
      max_str+= res;
      continue;
    }
    else if (wc == (my_wc_t) w_one)
    {
      if ((res= my_ci_wc_mb(cs, cs->min_sort_char,
                             (uchar*) min_str, (uchar*) min_end)) <= 0)
        goto pad_set_lengths;
      min_str+= res;

      if ((res= my_ci_wc_mb(cs, cs->max_sort_char,
                             (uchar*) max_str, (uchar*) max_end)) <= 0)
        goto pad_set_lengths;
      max_str+= res;
      continue;
    }
    else if (wc == (my_wc_t) w_many)
    {
      /*
        Calculate length of keys:
        a\min\min... is the smallest possible string
        a\max\max... is the biggest possible string
      */
      *min_length= (cs->state & (MY_CS_BINSORT | MY_CS_NOPAD)) ?
                    (size_t) (min_str - min_org) :
                    res_length;
      *max_length= res_length;
      goto pad_min_max;
    }

    if (contractions &&
        my_uca_can_be_contraction_head(contractions, wc) &&
        (res= my_ci_mb_wc(cs, &wc2, (uchar*) ptr, (uchar*) end)) > 0)
    {
      const uint16 *weight;
      if ((wc2 == (my_wc_t) w_one || wc2 == (my_wc_t) w_many))
      {
        /* Contraction head followed by a wildcard */
        *min_length= *max_length= res_length;
        goto pad_min_max;
      }

      if (my_uca_can_be_contraction_tail(contractions, wc2) &&
          (weight= my_uca_contraction2_weight(contractions, wc, wc2)) && weight[0])
      {
        /* Contraction found */
        if (charlen == 1)
        {
          /* contraction does not fit to result */
          *min_length= *max_length= res_length;
          goto pad_min_max;
        }

        ptr+= res;
        charlen--;

        /* Put contraction head */
        if ((res= my_ci_wc_mb(cs, wc, (uchar*) min_str, (uchar*) min_end)) <= 0)
          goto pad_set_lengths;
        min_str+= res;

        if ((res= my_ci_wc_mb(cs, wc, (uchar*) max_str, (uchar*) max_end)) <= 0)
          goto pad_set_lengths;
        max_str+= res;
        wc= wc2; /* Prepare to put contraction tail */
      }
    }

    /* Normal character, or contraction tail */
    if ((res= my_ci_wc_mb(cs, wc, (uchar*) min_str, (uchar*) min_end)) <= 0)
      goto pad_set_lengths;
    min_str+= res;
    if ((res= my_ci_wc_mb(cs, wc, (uchar*) max_str, (uchar*) max_end)) <= 0)
      goto pad_set_lengths;
    max_str+= res;
  }

pad_set_lengths:
  *min_length= (size_t) (min_str - min_org);
  *max_length= (size_t) (max_str - max_org);

pad_min_max:
  /*
    Fill up max_str and min_str to res_length.
    fill() cannot set incomplete characters and
    requires that "length" argument is divisible to mbminlen.
    Make sure to call fill() with proper "length" argument.
  */
  res_length_diff= res_length % cs->mbminlen;
  my_ci_fill(cs, min_str, min_end - min_str - res_length_diff,
                 cs->min_sort_char);
  my_ci_fill(cs, max_str, max_end - max_str - res_length_diff,
                 cs->max_sort_char);

  /* In case of incomplete characters set the remainder to 0x00's */
  if (res_length_diff)
  {
    /* Example: odd res_length for ucs2 */
    memset(min_end - res_length_diff, 0, res_length_diff);
    memset(max_end - res_length_diff, 0, res_length_diff);
  }
  return FALSE;
}


static int my_wildcmp_mb_bin_impl(CHARSET_INFO *cs,
                                  const char *str,const char *str_end,
                                  const char *wildstr,const char *wildend,
                                  int escape, int w_one, int w_many, int recurse_level)
{
  int result= -1;				/* Not found, using wildcards */

  if (my_string_stack_guard && my_string_stack_guard(recurse_level))
    return 1;
  while (wildstr != wildend)
  {
    while (*wildstr != w_many && *wildstr != w_one)
    {
      int l;
      if (*wildstr == escape && wildstr+1 != wildend)
	wildstr++;
      if ((l = my_ismbchar(cs, wildstr, wildend)))
      {
	  if (str+l > str_end || memcmp(str, wildstr, l) != 0)
	      return 1;
	  str += l;
	  wildstr += l;
      }
      else
      if (str == str_end || *wildstr++ != *str++)
	return(1);				/* No match */
      if (wildstr == wildend)
	return (str != str_end);		/* Match if both are at end */
      result=1;					/* Found an anchor char */
    }
    if (*wildstr == w_one)
    {
      do
      {
	if (str == str_end)			/* Skip one char if possible */
	  return (result);
	INC_PTR(cs,str,str_end);
      } while (++wildstr < wildend && *wildstr == w_one);
      if (wildstr == wildend)
	break;
    }
    if (*wildstr == w_many)
    {						/* Found w_many */
      int cmp;
      const char* mb = wildstr;
      int mb_len=0;
      
      wildstr++;
      /* Remove any '%' and '_' from the wild search string */
      for (; wildstr != wildend ; wildstr++)
      {
	if (*wildstr == w_many)
	  continue;
	if (*wildstr == w_one)
	{
	  if (str == str_end)
	    return (-1);
	  INC_PTR(cs,str,str_end);
	  continue;
	}
	break;					/* Not a wild character */
      }
      if (wildstr == wildend)
	return(0);				/* Ok if w_many is last */
      if (str == str_end)
	return -1;
      
      if ((cmp= *wildstr) == escape && wildstr+1 != wildend)
	cmp= *++wildstr;
	
      mb=wildstr;
      mb_len= my_ismbchar(cs, wildstr, wildend);
      INC_PTR(cs,wildstr,wildend);		/* This is compared trough cmp */
      do
      {
        for (;;)
        {
          if (str >= str_end)
            return -1;
          if (mb_len)
          {
            if (str+mb_len <= str_end && memcmp(str, mb, mb_len) == 0)
            {
              str += mb_len;
              break;
            }
          }
          else if (!my_ismbchar(cs, str, str_end) && *str == cmp)
          {
            str++;
            break;
          }
          INC_PTR(cs,str, str_end);
        }
	{
	  int tmp=my_wildcmp_mb_bin_impl(cs,str,str_end,
                                         wildstr,wildend,escape,
                                         w_one,w_many, recurse_level+1);
	  if (tmp <= 0)
	    return (tmp);
	}
      } while (str != str_end);
      return(-1);
    }
  }
  return (str != str_end ? 1 : 0);
}

int
my_wildcmp_mb_bin(CHARSET_INFO *cs,
                  const char *str,const char *str_end,
                  const char *wildstr,const char *wildend,
                  int escape, int w_one, int w_many)
{
  return my_wildcmp_mb_bin_impl(cs, str, str_end,
                                wildstr, wildend,
                                escape, w_one, w_many, 1);
}


/*
  Data was produced from EastAsianWidth.txt 
  using utt11-dump utility.
*/
static const char pg11[256]=
{
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,1,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

static const char pg23[256]=
{
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

static const char pg2E[256]=
{
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,1,1,1,1,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0
};

static const char pg2F[256]=
{
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0
};

static const char pg30[256]=
{
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,
0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,1,1,1,1,1,1,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1
};

static const char pg31[256]=
{
0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1
};

static const char pg32[256]=
{
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0
};

static const char pg4D[256]=
{
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

static const char pg9F[256]=
{
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

static const char pgA4[256]=
{
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

static const char pgD7[256]=
{
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

static const char pgFA[256]=
{
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

static const char pgFE[256]=
{
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,1,1,1,1,1,1,1,1,1,1,1,1,
1,1,1,1,1,1,1,0,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

static const char pgFF[256]=
{
0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

static const struct {int page; const char *p;} utr11_data[256]=
{
{0,NULL},{0,NULL},{0,NULL},{0,NULL},{0,NULL},{0,NULL},{0,NULL},{0,NULL},
{0,NULL},{0,NULL},{0,NULL},{0,NULL},{0,NULL},{0,NULL},{0,NULL},{0,NULL},
{0,NULL},{0,pg11},{0,NULL},{0,NULL},{0,NULL},{0,NULL},{0,NULL},{0,NULL},
{0,NULL},{0,NULL},{0,NULL},{0,NULL},{0,NULL},{0,NULL},{0,NULL},{0,NULL},
{0,NULL},{0,NULL},{0,NULL},{0,pg23},{0,NULL},{0,NULL},{0,NULL},{0,NULL},
{0,NULL},{0,NULL},{0,NULL},{0,NULL},{0,NULL},{0,NULL},{0,pg2E},{0,pg2F},
{0,pg30},{0,pg31},{0,pg32},{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},
{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},
{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},
{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},{0,pg4D},{1,NULL},{1,NULL},
{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},
{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},
{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},
{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},
{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},
{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},
{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},
{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},
{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},
{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},{0,pg9F},
{1,NULL},{1,NULL},{1,NULL},{1,NULL},{0,pgA4},{0,NULL},{0,NULL},{0,NULL},
{0,NULL},{0,NULL},{0,NULL},{0,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},
{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},
{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},
{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},
{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},
{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},{1,NULL},{0,pgD7},
{0,NULL},{0,NULL},{0,NULL},{0,NULL},{0,NULL},{0,NULL},{0,NULL},{0,NULL},
{0,NULL},{0,NULL},{0,NULL},{0,NULL},{0,NULL},{0,NULL},{0,NULL},{0,NULL},
{0,NULL},{0,NULL},{0,NULL},{0,NULL},{0,NULL},{0,NULL},{0,NULL},{0,NULL},
{0,NULL},{0,NULL},{0,NULL},{0,NULL},{0,NULL},{0,NULL},{0,NULL},{0,NULL},
{0,NULL},{1,NULL},{0,pgFA},{0,NULL},{0,NULL},{0,NULL},{0,pgFE},{0,pgFF}
};


size_t my_numcells_mb(CHARSET_INFO *cs, const char *b, const char *e)
{
  my_wc_t wc;
  size_t clen= 0;
  
  while (b < e)
  {
    int mb_len;
    uint pg;
    if ((mb_len= my_ci_mb_wc(cs, &wc, (uchar*) b, (uchar*) e)) <= 0)
    {
      mb_len= 1; /* Let's think a wrong sequence takes 1 dysplay cell */
      b++;
      continue;
    }
    b+= mb_len;
    if (wc > 0xFFFF)
    {
      if (wc >= 0x20000 && wc <= 0x3FFFD) /* CJK Ideograph Extension B, C */
        clen+= 1;
    }
    else
    {
      pg= (wc >> 8) & 0xFF;
      clen+= utr11_data[pg].p ? utr11_data[pg].p[wc & 0xFF] : utr11_data[pg].page;
    }
    clen++;
  }
  return clen;
}


int my_mb_ctype_mb(CHARSET_INFO *cs, int *ctype,
                   const uchar *s, const uchar *e)
{
  my_wc_t wc;
  int res= my_ci_mb_wc(cs, &wc, s, e);
  if (res <= 0 || wc > 0xFFFF)
    *ctype= 0;
  else
    *ctype= my_uni_ctype[wc>>8].ctype ?
            my_uni_ctype[wc>>8].ctype[wc&0xFF] :
            my_uni_ctype[wc>>8].pctype;    
  return res;
}


#endif
