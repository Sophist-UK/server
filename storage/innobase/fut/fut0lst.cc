/*****************************************************************************

Copyright (c) 1995, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2019, 2022, MariaDB Corporation.

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

/******************************************************************//**
@file fut/fut0lst.cc
File-based list utilities

Created 11/28/1995 Heikki Tuuri
***********************************************************************/

#include "fut0lst.h"
#include "buf0buf.h"
#include "page0page.h"


/** Write a file address.
@param[in]      block   file page
@param[in,out]  faddr   file address location
@param[in]      page    page number
@param[in]      boffset byte offset
@param[in,out]  mtr     mini-transaction */
static void flst_write_addr(const buf_block_t& block, byte *faddr,
                            uint32_t page, uint16_t boffset, mtr_t* mtr)
{
  ut_ad(mtr->memo_contains_page_flagged(faddr, MTR_MEMO_PAGE_X_FIX |
                                        MTR_MEMO_PAGE_SX_FIX));
  ut_a(page == FIL_NULL || boffset >= FIL_PAGE_DATA);
  ut_a(ut_align_offset(faddr, srv_page_size) >= FIL_PAGE_DATA);

  static_assert(FIL_ADDR_PAGE == 0, "compatibility");
  static_assert(FIL_ADDR_BYTE == 4, "compatibility");
  static_assert(FIL_ADDR_SIZE == 6, "compatibility");

  const bool same_page= mach_read_from_4(faddr + FIL_ADDR_PAGE) == page;
  const bool same_offset= mach_read_from_2(faddr + FIL_ADDR_BYTE) == boffset;
  if (same_page)
  {
    if (!same_offset)
      mtr->write<2>(block, faddr + FIL_ADDR_BYTE, boffset);
    return;
  }
  if (same_offset)
    mtr->write<4>(block, faddr + FIL_ADDR_PAGE, page);
  else
  {
    alignas(4) byte fil_addr[6];
    mach_write_to_4(fil_addr + FIL_ADDR_PAGE, page);
    mach_write_to_2(fil_addr + FIL_ADDR_BYTE, boffset);
    mtr->memcpy(block, faddr + FIL_ADDR_PAGE, fil_addr, 6);
  }
}

/** Write 2 null file addresses.
@param[in]      b       file page
@param[in,out]  addr    file address to be zeroed out
@param[in,out]  mtr     mini-transaction */
static void flst_zero_both(const buf_block_t& b, byte *addr, mtr_t *mtr)
{
  if (mach_read_from_4(addr + FIL_ADDR_PAGE) != FIL_NULL)
    mtr->memset(&b, addr + FIL_ADDR_PAGE, 4, 0xff);
  mtr->write<2,mtr_t::MAYBE_NOP>(b, addr + FIL_ADDR_BYTE, 0U);
  /* Initialize the other address by (MEMMOVE|0x80,offset,FIL_ADDR_SIZE,source)
  which is 4 bytes, or less than FIL_ADDR_SIZE. */
  memcpy(addr + FIL_ADDR_SIZE, addr, FIL_ADDR_SIZE);
  mtr->memmove(b, addr + FIL_ADDR_SIZE, addr, FIL_ADDR_SIZE);
}

/** Add a node to an empty list. */
static void flst_add_to_empty(buf_block_t *base, byte *boffset,
                              buf_block_t *add, byte *aoffset, mtr_t *mtr)
{
  ut_ad(base != add || boffset != aoffset);
  ut_ad(base->page.is_physical_address(boffset));
  ut_ad(add->page.is_physical_address(aoffset));
  ut_ad(mtr->memo_contains_flagged(base, MTR_MEMO_PAGE_X_FIX |
                                   MTR_MEMO_PAGE_SX_FIX));
  ut_ad(mtr->memo_contains_flagged(add, MTR_MEMO_PAGE_X_FIX |
                                   MTR_MEMO_PAGE_SX_FIX));

  ut_ad(!mach_read_from_4(boffset + FLST_LEN));
  mtr->write<1>(*base, boffset + (FLST_LEN + 3), 1U);
  /* Update first and last fields of base node */
  flst_write_addr(*base, boffset + FLST_FIRST,
                  add->page.id().page_no(), page_offset(aoffset), mtr);
  memcpy(boffset + FLST_LAST, boffset + FLST_FIRST, FIL_ADDR_SIZE);
  /* Initialize FLST_LAST by (MEMMOVE|0x80,offset,FIL_ADDR_SIZE,source)
  which is 4 bytes, or less than FIL_ADDR_SIZE. */
  mtr->memmove(*base, boffset + FLST_LAST, boffset + FLST_FIRST,
               FIL_ADDR_SIZE);

  /* Set prev and next fields of node to add */
  static_assert(FLST_NEXT == FLST_PREV + FIL_ADDR_SIZE, "compatibility");
  flst_zero_both(*add, aoffset + FLST_PREV, mtr);
}

/** Insert a node after another one.
@param[in,out]  base    base node block
@param[in]      boffset byte offset of the base node
@param[in,out]  cur     insert position block
@param[in]      coffset byte offset of the insert position
@param[in,out]  add     block to be added
@param[in]      aoffset byte offset of the block to be added
@param[in,out]  mtr     mini-transaction */
static dberr_t flst_insert_after(buf_block_t *base, byte *boffset,
                                 buf_block_t *cur, byte *coffset,
                                 buf_block_t *add, byte *aoffset,
                                 mtr_t *mtr)
{
  ut_ad(boffset != coffset);
  ut_ad(boffset != aoffset);
  ut_ad(coffset != aoffset);
  ut_ad(base->page.is_physical_address(boffset));
  ut_ad(cur->page.is_physical_address(coffset));
  ut_ad(add->page.is_physical_address(aoffset));
  ut_ad(mtr->memo_contains_flagged(base, MTR_MEMO_PAGE_X_FIX |
                                   MTR_MEMO_PAGE_SX_FIX));
  ut_ad(mtr->memo_contains_flagged(cur, MTR_MEMO_PAGE_X_FIX |
                                   MTR_MEMO_PAGE_SX_FIX));
  ut_ad(mtr->memo_contains_flagged(add, MTR_MEMO_PAGE_X_FIX |
                                   MTR_MEMO_PAGE_SX_FIX));

  fil_addr_t next_addr= flst_get_next_addr(coffset);

  flst_write_addr(*add, aoffset + FLST_PREV,
                  cur->page.id().page_no(), page_offset(coffset), mtr);
  flst_write_addr(*add, aoffset + FLST_NEXT,
                  next_addr.page, next_addr.boffset, mtr);

  dberr_t err= DB_SUCCESS;

  const uint16_t a= page_offset(aoffset);

  if (next_addr.page == FIL_NULL)
    flst_write_addr(*base, boffset + FLST_LAST,
                    add->page.id().page_no(), a, mtr);
  else if (buf_block_t *block=
           buf_page_get_gen(page_id_t{add->page.id().space(), next_addr.page},
                            add->zip_size(), RW_SX_LATCH, nullptr,
                            BUF_GET_POSSIBLY_FREED, mtr, &err))
    flst_write_addr(*block, block->page.frame +
                    next_addr.boffset + FLST_PREV,
                    add->page.id().page_no(), a, mtr);

  flst_write_addr(*cur, coffset + FLST_NEXT, add->page.id().page_no(), a, mtr);

  byte *len= &boffset[FLST_LEN];
  mtr->write<4>(*base, len, mach_read_from_4(len) + 1);
  return err;
}

/** Insert a node before another one.
@param[in,out]  base    base node block
@param[in]      boffset byte offset of the base node
@param[in,out]  cur     insert position block
@param[in]      coffset byte offset of the insert position
@param[in,out]  add     block to be added
@param[in]      aoffset byte offset of the block to be added
@param[in,out]  mtr     mini-transaction
@return error code */
static dberr_t flst_insert_before(buf_block_t *base, byte *boffset,
                                  buf_block_t *cur, byte *coffset,
                                  buf_block_t *add, byte *aoffset,
                                  mtr_t *mtr)
{
  ut_ad(boffset != coffset);
  ut_ad(boffset != aoffset);
  ut_ad(coffset != aoffset);
  ut_ad(base->page.is_physical_address(boffset));
  ut_ad(cur->page.is_physical_address(coffset));
  ut_ad(add->page.is_physical_address(aoffset));
  ut_ad(mtr->memo_contains_flagged(base, MTR_MEMO_PAGE_X_FIX |
                                   MTR_MEMO_PAGE_SX_FIX));
  ut_ad(mtr->memo_contains_flagged(cur, MTR_MEMO_PAGE_X_FIX |
                                   MTR_MEMO_PAGE_SX_FIX));
  ut_ad(mtr->memo_contains_flagged(add, MTR_MEMO_PAGE_X_FIX |
                                   MTR_MEMO_PAGE_SX_FIX));

  fil_addr_t prev_addr= flst_get_prev_addr(coffset);

  flst_write_addr(*add, aoffset + FLST_PREV,
                  prev_addr.page, prev_addr.boffset, mtr);
  flst_write_addr(*add, aoffset + FLST_NEXT,
                  cur->page.id().page_no(), page_offset(coffset), mtr);

  dberr_t err= DB_SUCCESS;
  const uint16_t a= page_offset(aoffset);

  if (prev_addr.page == FIL_NULL)
    flst_write_addr(*base, boffset + FLST_FIRST,
                    add->page.id().page_no(), a, mtr);
  else if (buf_block_t *block=
           buf_page_get_gen(page_id_t{add->page.id().space(), prev_addr.page},
                            add->zip_size(), RW_SX_LATCH, nullptr,
                            BUF_GET_POSSIBLY_FREED, mtr, &err))
    flst_write_addr(*block, block->page.frame +
                    prev_addr.boffset + FLST_NEXT,
                    add->page.id().page_no(), a, mtr);

  flst_write_addr(*cur, coffset + FLST_PREV, add->page.id().page_no(), a, mtr);

  byte *len= &boffset[FLST_LEN];
  mtr->write<4>(*base, len, mach_read_from_4(len) + 1);
  return err;
}

/** Initialize a list base node.
@param[in]      block   file page
@param[in,out]  base    base node
@param[in,out]  mtr     mini-transaction */
void flst_init(const buf_block_t& block, byte *base, mtr_t *mtr)
{
  ut_ad(mtr->memo_contains_page_flagged(base, MTR_MEMO_PAGE_X_FIX |
                                        MTR_MEMO_PAGE_SX_FIX));
  mtr->write<4,mtr_t::MAYBE_NOP>(block, base + FLST_LEN, 0U);
  static_assert(FLST_LAST == FLST_FIRST + FIL_ADDR_SIZE, "compatibility");
  flst_zero_both(block, base + FLST_FIRST, mtr);
}

/** Append a file list node to a list.
@param[in,out]  base    base node block
@param[in]      boffset byte offset of the base node
@param[in,out]  add     block to be added
@param[in]      aoffset byte offset of the node to be added
@param[in,outr] mtr     mini-transaction */
dberr_t flst_add_last(buf_block_t *base, byte *boffset,
                      buf_block_t *add, byte *aoffset, mtr_t *mtr)
{
  ut_ad(boffset != aoffset);
  ut_ad(base->page.is_physical_address(boffset));
  ut_ad(add->page.is_physical_address(aoffset));
  ut_ad(mtr->memo_contains_flagged(base, MTR_MEMO_PAGE_X_FIX |
                                   MTR_MEMO_PAGE_SX_FIX));
  ut_ad(mtr->memo_contains_flagged(add, MTR_MEMO_PAGE_X_FIX |
                                   MTR_MEMO_PAGE_SX_FIX));
  if (!flst_get_len(boffset))
  {
    flst_add_to_empty(base, boffset, add, aoffset, mtr);
    return DB_SUCCESS;
  }
  else
  {
    fil_addr_t addr= flst_get_last(boffset);
    buf_block_t *cur= add;
    byte *cur_page;
    if (addr.page != add->page.id().page_no())
    {
      dberr_t err;
      cur= buf_page_get_gen(page_id_t{add->page.id().space(), addr.page},
                            add->zip_size(), RW_SX_LATCH, nullptr,
                            BUF_GET_POSSIBLY_FREED, mtr, &err);
      if (!cur)
        return err;
      cur_page= cur->page.frame;
    }
    else
      cur_page= page_align(aoffset);
    return flst_insert_after(base, boffset, cur, cur_page + addr.boffset,
                             add, aoffset, mtr);
  }
}

/** Prepend a file list node to a list.
@param[in,out]  base    base node block
@param[in]      boffset byte offset of the base node
@param[in,out]  add     block to be added
@param[in]      aoffset byte offset of the node to be added
@param[in,out]  mtr     mini-transaction
@return error code */
dberr_t flst_add_first(buf_block_t *base, byte *boffset,
                       buf_block_t *add, byte *aoffset, mtr_t *mtr)
{
  ut_ad(base != add || boffset != aoffset);
  ut_ad(base->page.is_physical_address(boffset));
  ut_ad(add->page.is_physical_address(aoffset));
  ut_ad(mtr->memo_contains_flagged(base, MTR_MEMO_PAGE_X_FIX |
                                   MTR_MEMO_PAGE_SX_FIX));
  ut_ad(mtr->memo_contains_flagged(add, MTR_MEMO_PAGE_X_FIX |
                                   MTR_MEMO_PAGE_SX_FIX));

  if (!flst_get_len(boffset))
  {
    flst_add_to_empty(base, boffset, add, aoffset, mtr);
    return DB_SUCCESS;
  }
  else
  {
    fil_addr_t addr= flst_get_first(boffset);
    buf_block_t *cur= add;
    byte *cur_page;
    if (addr.page != add->page.id().page_no())
    {
      dberr_t err;
      cur= buf_page_get_gen(page_id_t{add->page.id().space(), addr.page},
                            add->zip_size(), RW_SX_LATCH, nullptr,
                            BUF_GET_POSSIBLY_FREED, mtr, &err);
      if (!cur)
        return err;
      cur_page= cur->page.frame;
    }
    else
      cur_page= page_align(aoffset);
    return flst_insert_before(base, boffset, cur, cur_page + addr.boffset,
                              add, aoffset, mtr);
  }
}

/** Remove a file list node.
@param[in,out]  base    base node block
@param[in]      boffset byte offset of the base node
@param[in,out]  cur     block to be removed
@param[in]      coffset byte offset of the current record to be removed
@param[in,out]  mtr     mini-transaction
@return error code */
dberr_t flst_remove(buf_block_t *base, byte *boffset,
                    buf_block_t *cur, byte *coffset, mtr_t *mtr)
{
  ut_ad(base->page.is_physical_address(boffset));
  ut_ad(cur->page.is_physical_address(coffset));
  ut_ad(mtr->memo_contains_flagged(base, MTR_MEMO_PAGE_X_FIX |
                                   MTR_MEMO_PAGE_SX_FIX));
  ut_ad(mtr->memo_contains_flagged(cur, MTR_MEMO_PAGE_X_FIX |
                                   MTR_MEMO_PAGE_SX_FIX));

  const fil_addr_t prev_addr= flst_get_prev_addr(coffset);
  const fil_addr_t next_addr= flst_get_next_addr(coffset);
  dberr_t err= DB_SUCCESS;

  if (prev_addr.page == FIL_NULL)
    flst_write_addr(*base, boffset + FLST_FIRST,
                    next_addr.page, next_addr.boffset, mtr);
  else
  {
    do
    {
      buf_block_t *b= cur;
      byte *frame;
      if (prev_addr.page != b->page.id().page_no())
      {
        b= buf_page_get_gen(page_id_t(b->page.id().space(), prev_addr.page),
                            b->zip_size(), RW_SX_LATCH, nullptr,
                            BUF_GET_POSSIBLY_FREED, mtr, &err);
        if (UNIV_UNLIKELY(!b))
          continue;
        frame= b->page.frame;
      }
      else
        frame= page_align(coffset);
      flst_write_addr(*b, frame + prev_addr.boffset + FLST_NEXT,
                      next_addr.page, next_addr.boffset, mtr);
    }
    while (0);
  }

  if (next_addr.page == FIL_NULL)
    flst_write_addr(*base, boffset + FLST_LAST,
                    prev_addr.page, prev_addr.boffset, mtr);
  else
  {
    do
    {
      if (next_addr.page != cur->page.id().page_no())
      {
        dberr_t err2;
        cur= buf_page_get_gen(page_id_t(cur->page.id().space(),
                                        next_addr.page),
                              cur->zip_size(), RW_SX_LATCH, nullptr,
                              BUF_GET_POSSIBLY_FREED, mtr, &err2);
        if (!cur)
        {
          if (err == DB_SUCCESS)
            err= err2;
          continue;
        }

        coffset= cur->page.frame;
      }
      else
        coffset= page_align(coffset);

      flst_write_addr(*cur, coffset + next_addr.boffset + FLST_PREV,
                      prev_addr.page, prev_addr.boffset, mtr);
    }
    while (0);
  }

  byte *len= &boffset[FLST_LEN];
  if (UNIV_UNLIKELY(!mach_read_from_4(len)))
    return DB_CORRUPTION;
  mtr->write<4>(*base, len, mach_read_from_4(len) - 1);
  return err;
}

#ifdef UNIV_DEBUG
/** Validate a file-based list. */
void flst_validate(const buf_block_t *base, uint16_t boffset, mtr_t *mtr)
{
  ut_ad(boffset < base->physical_size());
  ut_ad(mtr->memo_contains_flagged(base, MTR_MEMO_PAGE_X_FIX |
                                   MTR_MEMO_PAGE_SX_FIX));

  /* We use two mini-transaction handles: the first is used to lock
  the base node, and prevent other threads from modifying the list.
  The second is used to traverse the list. We cannot run the second
  mtr without committing it at times, because if the list is long,
  the x-locked pages could fill the buffer, resulting in a deadlock. */
  mtr_t mtr2;
  const byte *flst= base->page.frame + boffset;
  const uint32_t len= flst_get_len(flst);
  fil_addr_t addr= flst_get_first(flst);

  for (uint32_t i= len; i--; )
  {
    mtr2.start();
    const buf_block_t *b=
      buf_page_get_gen(page_id_t(base->page.id().space(), addr.page),
                       base->zip_size(), RW_SX_LATCH, nullptr, BUF_GET, mtr);
    ut_ad(b);
    addr= flst_get_next_addr(b->page.frame + addr.boffset);
    mtr2.commit();
  }

  ut_ad(addr.page == FIL_NULL);

  addr= flst_get_last(flst);

  for (uint32_t i= len; i--; )
  {
    mtr2.start();
    const buf_block_t *b=
      buf_page_get_gen(page_id_t(base->page.id().space(), addr.page),
                       base->zip_size(), RW_SX_LATCH, nullptr, BUF_GET, mtr);
    ut_ad(b);
    addr= flst_get_prev_addr(b->page.frame + addr.boffset);
    mtr2.commit();
  }

  ut_ad(addr.page == FIL_NULL);
}
#endif
