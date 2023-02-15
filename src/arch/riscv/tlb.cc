/*
 * Copyright (c) 2001-2005 The Regents of The University of Michigan
 * Copyright (c) 2007 MIPS Technologies, Inc.
 * Copyright (c) 2020 Barkhausen Institut
 * Copyright (c) 2021 Huawei International
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "arch/riscv/tlb.hh"

#include <string>
#include <vector>

#include "arch/riscv/faults.hh"
#include "arch/riscv/mmu.hh"
#include "arch/riscv/pagetable.hh"
#include "arch/riscv/pagetable_walker.hh"
#include "arch/riscv/pma_checker.hh"
#include "arch/riscv/pmp.hh"
#include "arch/riscv/pra_constants.hh"
#include "arch/riscv/utility.hh"
#include "base/inifile.hh"
#include "base/str.hh"
#include "base/trace.hh"
#include "cpu/thread_context.hh"
#include "debug/TLB.hh"
#include "debug/TLBVerbose.hh"
#include "debug/TLBVerbosel2.hh"
#include "mem/page_table.hh"
#include "params/RiscvTLB.hh"
#include "sim/full_system.hh"
#include "sim/process.hh"
#include "sim/system.hh"

namespace gem5
{

using namespace RiscvISA;

///////////////////////////////////////////////////////////////////////
//
//  RISC-V TLB
//

static Addr
buildKey(Addr vpn, uint16_t asid)
{
    return (static_cast<Addr>(asid) << 48) | vpn;
}

TLB::TLB(const Params &p) :
    BaseTLB(p), size(p.size),l2tlb_l1_size(p.l2tlb_l1_size),
    l2tlb_l2_size(p.l2tlb_l2_size),l2tlb_l3_size(p.l2tlb_l3_size),
    l2tlb_sp_size(p.l2tlb_sp_size),
    tlb(size),lruSeq(0),hit_in_sp(false),stats(this), pma(p.pma_checker),
    pmp(p.pmp),
    tlb_l2l1(l2tlb_l1_size *8 ),tlb_l2l2(l2tlb_l2_size *8),
    tlb_l2l3(l2tlb_l3_size*8),tlb_l2sp(l2tlb_sp_size*8)
{
    DPRINTF(TLBVerbose, "tlb11\n");

    for (size_t x = 0; x < size; x++) {
        tlb[x].trieHandle = NULL;
        freeList.push_back(&tlb[x]);
    }

    for (size_t x_l2l1 = 0;x_l2l1 < l2tlb_l1_size*8 ;x_l2l1++){
        tlb_l2l1[x_l2l1].trieHandle = NULL;
        freeList_l2l1.push_back(&tlb_l2l1[x_l2l1]);
    }

    for (size_t x_l2l2 = 0;x_l2l2 < l2tlb_l2_size*8 ;x_l2l2++){
        tlb_l2l2[x_l2l2].trieHandle = NULL;
        freeList_l2l2.push_back(&tlb_l2l2[x_l2l2]);
    }
    for (size_t x_l2l3 = 0;x_l2l3 < l2tlb_l3_size*8 ;x_l2l3++){
        tlb_l2l3[x_l2l3].trieHandle = NULL;
        freeList_l2l3.push_back(&tlb_l2l3[x_l2l3]);
    }
    for (size_t x_l2sp = 0;x_l2sp < l2tlb_sp_size*8 ; x_l2sp++){
        tlb_l2sp[x_l2sp].trieHandle = NULL;
        freeList_l2sp.push_back(&tlb_l2sp[x_l2sp]);
    }

    DPRINTF(TLBVerbose, "tlb11 tlb_size %d\n",size);

    walker = p.walker;
    walker->setTLB(this);
}

Walker *
TLB::getWalker()
{
    return walker;
}

void
TLB::evictLRU()
{
    // Find the entry with the lowest (and hence least recently updated)
    // sequence number.

    size_t lru = 0;
    for (size_t i = 1; i < size; i++) {
        if (tlb[i].lruSeq < tlb[lru].lruSeq)
            lru = i;
    }

    remove(lru);
}

void
TLB::l2TLB_evictLRU(int l2TLBlevel,Addr vaddr){
    size_t lru;
    size_t i;
    Addr l2_index;
    Addr l3_index;
    l2_index = (vaddr >> 24) & (0x1f);
    l3_index = (vaddr >> 15) & (0x7f);
    int l2_index_num = 0;
    int l3_index_num = 0;
    if (l2TLBlevel == 1) {
        lru =0;
        for (i = 8;i< l2tlb_l1_size*8;i=i+8){
            if (tlb_l2l1[i].lruSeq < tlb_l2l1[lru].lruSeq){
                lru = i;
            }
        }
        l2TLB_remove(lru,1,0,0,0);
    }

    else if (l2TLBlevel == 2) {
        lru = 0;
        for (i = 0; i < l2tlb_l2_size * 8; i = i + 8) {
            if ((tlb_l2l2[i].index == l2_index) &&
                (tlb_l2l2[i].trieHandle != NULL)) {
                DPRINTF(TLBVerbose, "vaddr %#x index %#x\n",
                        tlb_l2l2[i].vaddr, l2_index);
                if (l2_index_num == 0) {
                    lru = i;
                } else if (tlb_l2l2[i].lruSeq < tlb_l2l2[lru].lruSeq) {
                    lru = i;
                }
                l2_index_num++;
            }
        }
        if (l2_index_num == 2)
            l2TLB_remove(lru, 0, 1, 0, 0);
        else if (l2_index_num > 2) {
            printf("l2_index_num %d\n", l2_index_num);
            assert(0);
        }

    }

    else if (l2TLBlevel == 3) {
        lru = 0;
        for (i = 0; i < l2tlb_l3_size * 8; i = i + 8) {
            if ((tlb_l2l3[i].index == l3_index) &&
                (tlb_l2l3[i].trieHandle != NULL)) {
                if (l3_index_num == 0) {
                    lru = i;
                } else if (tlb_l2l3[i].lruSeq < tlb_l2l3[lru].lruSeq) {
                    lru = i;
                }
                l3_index_num++;
            }
        }
        if (l3_index_num == 4)
            l2TLB_remove(lru, 0, 0, 1, 0);
        else if (l3_index_num > 4)
            assert(0);

    }

    else if ((l2TLBlevel == 4) || (l2TLBlevel == 5)) {
        lru =0;
        for (i = 8;i< l2tlb_sp_size*8;i=i+8){
            if (tlb_l2sp[i].lruSeq < tlb_l2sp[lru].lruSeq){
                lru = i;
            }
        }
        l2TLB_remove(lru,0,0,0,1);
    }
}

TlbEntry *
TLB::lookup(Addr vpn, uint16_t asid, BaseMMU::Mode mode, bool hidden)
{
    TlbEntry *entry = trie.lookup(buildKey(vpn, asid));

    if (!hidden) {
        if (entry)
            entry->lruSeq = nextSeq();

        if (mode == BaseMMU::Write)
            stats.writeAccesses++;
        else
            stats.readAccesses++;

        if (!entry) {
            if (mode == BaseMMU::Write)
                stats.writeMisses++;
            else
                stats.readMisses++;
        }
        else {
            if (mode == BaseMMU::Write)
                stats.writeHits++;
            else
                stats.readHits++;
        }

        DPRINTF(TLBVerbose, "lookup(vpn=%#x, asid=%#x): %s ppn %#x\n",
                vpn, asid, entry ? "hit" : "miss", entry ? entry->paddr : 0);
    }

    return entry;
}

TlbEntry *
TLB::lookup_l2tlb(Addr vpn, uint16_t asid, BaseMMU::Mode mode, bool hidden,
                  int f_level)
{

    Addr f_vpnl2l1 = ((vpn >> 30)) << 30;
    Addr f_vpnl2l2 = ((vpn >> 21)) << 21;
    int i;

    DPRINTF(TLB, "f_vpnl2l1 %#x f_vpnl2l2 %#x vpn %#x\n",
            f_vpnl2l1, f_vpnl2l2,vpn);

    TlbEntry *entry_l2l1 = NULL;
    TlbEntry *entry_l2l2 = NULL;
    TlbEntry *entry_l2l3 = NULL;
    TlbEntry *entry_l2sp1 = NULL;
    TlbEntry *entry_l2sp2 = NULL;

    if (f_level == 1) {
        DPRINTF(TLB, "look up l2tlb in l2l1\n");
        DPRINTF(TLB,"key %#x\n",buildKey(f_vpnl2l1, asid));
        entry_l2l1 = trie_l2l1.lookup(buildKey(f_vpnl2l1, asid));
    }
    if (f_level == 2) {
        DPRINTF(TLB, "look up l2tlb in l2l2\n");
        entry_l2l2 = trie_l2l2.lookup(buildKey(f_vpnl2l2, asid));
    }
    if (f_level == 3) {
        DPRINTF(TLB, "look up l2tlb in l2l3\n");
        entry_l2l3 = trie_l2l3.lookup(buildKey(vpn, asid));
    }
    if (f_level == 4) {
        DPRINTF(TLB, "look up l2tlb in l2sp1\n");
        entry_l2sp1 = trie_l2sp.lookup(buildKey(f_vpnl2l1, asid));
        if (entry_l2sp1){
            if (entry_l2sp1->level == 1){
                DPRINTF(TLB,"hit in sp but sp2 , return\n");
                return NULL;
            }
        }
    }
    if (f_level == 5) {
        DPRINTF(TLB, "look up l2tlb in l2sp2\n");
        entry_l2sp2 = trie_l2sp.lookup(buildKey(f_vpnl2l2, asid));
        if (entry_l2sp2) {
            if (entry_l2sp2->level == 2) {
                DPRINTF(TLB, "hit in sp but sp1 , return\n");
                return NULL;
            }
        }
    }
    Addr step;
    if (!hidden) {
        if (!(entry_l2l1 || entry_l2l2 || entry_l2l3 || entry_l2sp1 ||
              entry_l2sp2)) {
            if (mode == BaseMMU::Write) {
                stats.writeL2TlbMisses++;
            } else {
                stats.ReadL2TlbMisses++;
            }

        } else {
            if (mode == BaseMMU::Write) {
                stats.writeL2TlbHits++;
            } else {
                stats.ReadL2TlbHits++;
            }
        }

        if (entry_l2l3) {
            Addr vpnl2l3 = ((vpn >> 15)) << 15;
            step = 0x1000;
            for (i = 0; i < 8; i++) {
                TlbEntry *m_entry_l2l3 =
                    trie_l2l3.lookup(buildKey((vpnl2l3 + step * i), asid));
                if (m_entry_l2l3 == NULL) {
                    DPRINTF(TLB, "l2l3 TLB link num is empty\n");
                    DPRINTF(TLB, "l2l3 vaddr basic %#x vaddr %#x\n", vpnl2l3,
                            vpnl2l3 + step * i);
                    assert(0);
                }
                m_entry_l2l3->lruSeq = nextSeq();
            }
            DPRINTF(TLBVerbose,
                    "lookup l2l3 (vpn=%#x, asid=%#x): %s ppn %#x\n", vpn, asid,
                    entry_l2l3 ? "hit" : "miss",
                    entry_l2l3 ? entry_l2l3->paddr : 0);
            return entry_l2l3;
        }

        if (entry_l2sp1) {
            Addr vpnl2sp1 = ((vpn >> 33) ) << 33;
            step = 0x1 << 30;

            DPRINTF(TLB,"l2sp1 hit hit vpn %#x,paddr %#x pte %#x\n",
                    entry_l2sp1->vaddr,entry_l2sp1->paddr,entry_l2sp1->pte);
            for (i = 0; i < 8; i++) {
                TlbEntry *m_entry_sp1 =
                    trie_l2sp.lookup(buildKey(vpnl2sp1 + step * i, asid));
                if (m_entry_sp1 == NULL) {
                    DPRINTF(TLB, "l2sp1 TLB link num is empty\n");
                    DPRINTF(TLB, "l2sp1 vaddr basic %#x vaddr %#x base %#x\n",
                            vpnl2sp1,vpnl2sp1 + step * i,vpn);
                    assert(0);
                }
                m_entry_sp1->lruSeq = nextSeq();
            }
            DPRINTF(TLBVerbose,
                    "lookup l2sp1 (vpn=%#x, asid=%#x): %s ppn %#x\n", vpn,
                    asid, entry_l2sp1 ? "hit" : "miss",
                    entry_l2sp1 ? entry_l2sp1->paddr : 0);
            return entry_l2sp1;
        }
        if (entry_l2sp2) {
            Addr vpnl2sp2 = ((vpn >> 24)) << 24;
            step = 0x1 << 21;
            for (i = 0; i < 8; i++) {
                TlbEntry *m_entry_sp2 =
                    trie_l2sp.lookup(buildKey(vpnl2sp2 + step * i, asid));
                if (m_entry_sp2 == NULL) {
                    DPRINTF(TLB, "l2sp2 TLB link num is empty\n");
                    DPRINTF(TLB, "l2sp2 vaddr basic %#x vaddr %#x\n", vpnl2sp2,
                            vpnl2sp2 + step * i);
                    assert(0);
                }
                m_entry_sp2->lruSeq = nextSeq();
            }
            DPRINTF(TLBVerbose,
                    "lookup l2sp2 (vpn=%#x, asid=%#x): %s ppn %#x\n", vpn,
                    asid, entry_l2sp2 ? "hit" : "miss",
                    entry_l2sp2 ? entry_l2sp2->paddr : 0);
            return entry_l2sp2;
        }

        if (entry_l2l2) {
            Addr vpnl2l2 = ((vpn >> 24)) << 24;
            step = 0x1 << 21;
            for (i = 0; i < 8; i++) {
                TlbEntry *m_entry_l2l2 =
                    trie_l2l2.lookup(buildKey(vpnl2l2 + step * i, asid));
                if (m_entry_l2l2 == NULL) {
                    DPRINTF(TLB, "l2l2 TLB link num is empty\n");
                    DPRINTF(TLB, "l2l2 vaddr basic %#x vaddr %#x\n", vpnl2l2,
                            vpnl2l2 + step * i);
                    assert(0);
                }
                m_entry_l2l2->lruSeq = nextSeq();
            }
            DPRINTF(TLBVerbose,
                    "lookup l2l2 (vpn=%#x, asid=%#x): %s ppn %#x\n", vpn, asid,
                    entry_l2l2 ? "hit" : "miss",
                    entry_l2l2 ? entry_l2l2->paddr : 0);
            return entry_l2l2;
        }

        if (entry_l2l1) {

            Addr vpnl2l1 = ((vpn >> 33) ) << 33;
            step = 0x1 << 30;
            //*hitlevel = 1;
            for (i = 0; i < 8; i++) {
                TlbEntry *m_entry_l2l1 =
                    trie_l2l1.lookup(buildKey(vpnl2l1 + step * i, asid));
                if (m_entry_l2l1 == NULL) {
                    DPRINTF(TLB, "l2l1 TLB link num is empty\n");
                    DPRINTF(TLB, "l2l1 vaddr basic %#x vaddr %#x\n", vpnl2l1,
                            vpnl2l1 + step * i);
                    assert(0);
                }
                m_entry_l2l1->lruSeq = nextSeq();
            }
            DPRINTF(TLBVerbose,
                    "lookup l2l1 (vpn=%#x, asid=%#x): %s ppn %#x\n", vpn, asid,
                    entry_l2l1 ? "hit" : "miss",
                    entry_l2l1 ? entry_l2l1->paddr : 0);
            return entry_l2l1;
        }

    } else {
        if (entry_l2l3)
            return entry_l2l3;
        else if (entry_l2sp2)
            return entry_l2sp2;
        else if (entry_l2sp1)
            return entry_l2sp1;
        else if (entry_l2l2)
            return entry_l2l2;
        else if (entry_l2l1)
            return entry_l2l1;
    }
    return NULL;
}

TlbEntry *
TLB::insert(Addr vpn, const TlbEntry &entry)
{
    DPRINTF(TLB, "insert(vpn=%#x, asid=%#x): ppn=%#x pte=%#x size=%#x\n",
        vpn, entry.asid, entry.paddr, entry.pte, entry.size());

    // If somebody beat us to it, just use that existing entry.
    TlbEntry *newEntry = lookup(vpn, entry.asid, BaseMMU::Read, false);
    if (newEntry) {
        // update PTE flags (maybe we set the dirty/writable flag)
        newEntry->pte = entry.pte;
        assert(newEntry->vaddr == vpn);
        return newEntry;
    }

    if (freeList.empty())
        evictLRU();

    newEntry = freeList.front();
    freeList.pop_front();

    Addr key = buildKey(vpn, entry.asid);
    *newEntry = entry;
    newEntry->lruSeq = nextSeq();
    newEntry->vaddr = vpn;
    newEntry->trieHandle =
    trie.insert(key, TlbEntryTrie::MaxBits - entry.logBytes, newEntry);
    DPRINTF(TLB, "trie insert key %#x logbytes %#x paddr %#x\n", key,
            entry.logBytes, newEntry->paddr);
    return newEntry;
}

TlbEntry *
TLB::L2TLB_insert_in(Addr vpn, const TlbEntry &entry, int choose,
                     EntryList *List, TlbEntryTrie *Trie_l2,int sign)
{
    DPRINTF(TLB,
            "l2tlb insert(vpn=%#x, vpn2 %#x asid=%#x): ppn=%#x pte=%#x "
            "size=%#x level %d\n",
            vpn, entry.vaddr, entry.asid, entry.paddr, entry.pte, entry.size(),
            choose);
    TlbEntry *newEntry;
    Addr key;
    newEntry = lookup_l2tlb(vpn, entry.asid, BaseMMU::Read, true, choose);
    if (newEntry) {
        newEntry->pte = entry.pte;
        if (newEntry->vaddr != vpn) {
            Addr newEntryAddr = buildKey(newEntry->vaddr,newEntry->asid);
            Addr vpnAddr = buildKey(entry.vaddr,entry.asid);
            DPRINTF(TLBVerbosel2, "newEntryAddr %#x vpnAddr %#x\n",
                    newEntryAddr, vpnAddr);
            DPRINTF(TLBVerbosel2,
                    "l2tlb insert(vpn=%#x, vpn2 %#x asid=%#x): ppn=%#x "
                    "pte=%#x size=%#x level %d\n",
                    vpn, entry.vaddr, entry.asid, entry.paddr, entry.pte,
                    entry.size(), choose);
            DPRINTF(TLBVerbosel2,
                    "newentry(vpn=%#x, vpn2 %#x asid=%#x): ppn=%#x pte=%#x "
                    "size=%#x level %d\n",
                    vpn, newEntry->vaddr, newEntry->asid, newEntry->paddr,
                    newEntry->pte, newEntry->size(), choose);
            DPRINTF(TLBVerbosel2, "newEntry->vaddr %#x vpn %#x choose %d\n",
                    newEntry->vaddr, vpn, choose);
            assert(0);
        }
        return newEntry;
    }
    DPRINTF(TLB, "not hit in l2 tlb\n");
    if ((choose == 2 || choose == 3) && (sign == 0)) {
        l2TLB_evictLRU(choose, vpn);
    } else {
        if ((*List).empty())
            l2TLB_evictLRU(choose, vpn);
    }

    newEntry = (*List).front();
    (*List).pop_front();


    key = buildKey(vpn, entry.asid);

    *newEntry = entry;
    newEntry->lruSeq = nextSeq();
    newEntry->vaddr = vpn;
    if (entry.paddr == 0) {
        DPRINTF(TLB, " l2tlb num is outside vaddr %#x paddr %#x \n",
                entry.vaddr, entry.paddr);
    }

    newEntry->trieHandle = (*Trie_l2).insert(
        key, TlbEntryTrie::MaxBits - entry.logBytes, newEntry);


    DPRINTF(TLB, "l2tlb trie insert key %#x logbytes %#x len %#x\n", key,
            entry.logBytes,TlbEntryTrie::MaxBits - entry.logBytes);

    return newEntry;

}

TlbEntry *
TLB::L2TLB_insert(Addr vpn, const TlbEntry &entry, int level, int choose,
                  int sign)
{
    TlbEntry *newEntry = NULL;
    DPRINTF(TLB, "choose %d vpn %#x entry->vaddr %#x\n", choose, vpn,
            entry.vaddr);
    if (choose == 1)
        newEntry = L2TLB_insert_in(vpn, entry, choose, &freeList_l2l1,
                                   &trie_l2l1, sign);
    else if (choose == 2)
        newEntry = L2TLB_insert_in(vpn, entry, choose, &freeList_l2l2,
                                   &trie_l2l2, sign);
    else if (choose == 3)
        newEntry = L2TLB_insert_in(vpn, entry, choose, &freeList_l2l3,
                                   &trie_l2l3, sign);
    else if (choose == 4)
        newEntry = L2TLB_insert_in(vpn, entry, choose, &freeList_l2sp,
                                   &trie_l2sp, sign);
    else if (choose == 5)
        newEntry = L2TLB_insert_in(vpn, entry, choose, &freeList_l2sp,
                                   &trie_l2sp, sign);

    assert(newEntry != nullptr);
    return newEntry;
}


void
TLB::demapPage(Addr vpn, uint64_t asid)
{
    asid &= 0xFFFF;

    size_t i;
    Addr vpnl2l1 = ((vpn >> 33) ) << 33;
    Addr vpnl2l2 = ((vpn >> 24) ) << 24;
    Addr vpnl2l3 = ((vpn >> 15) ) << 15;
    Addr vpnl2sp1 = ((vpn >> 33) ) << 33;
    Addr vpnl2sp2 = ((vpn >> 24) ) << 24;
    if (vpn == 0 && asid == 0) {
        flushAll();
    }

    else {
        DPRINTF(TLB, "flush(vpn=%#x, asid=%#x)\n", vpn, asid);
        DPRINTF(TLB, "l2tlb flush(vpn=%#x, asid=%#x)\n", vpn, asid);
        if (vpn != 0 && asid != 0) {
            TlbEntry *newEntry = lookup(vpn, asid, BaseMMU::Read, true);
            TlbEntry *l2l1_newEntry =
                lookup_l2tlb(vpn, asid, BaseMMU::Read, true, 1);
            TlbEntry *l2l2_newEntry =
                lookup_l2tlb(vpn, asid, BaseMMU::Read, true, 2);
            TlbEntry *l2l3_newEntry =
                lookup_l2tlb(vpn, asid, BaseMMU::Read, true, 3);
            TlbEntry *l2sp1_newEntry =
                lookup_l2tlb(vpn, asid, BaseMMU::Read, true, 4);
            TlbEntry *l2sp2_newEntry =
                lookup_l2tlb(vpn, asid, BaseMMU::Read, true, 5);

            if (newEntry)
                remove(newEntry - tlb.data());
            if (l2l1_newEntry) {
                TlbEntry *m_l2l1_newEntry =
                    lookup_l2tlb(vpnl2l1, asid, BaseMMU::Read, true, 1);
                if (m_l2l1_newEntry == NULL)
                    assert(0);
                l2TLB_remove(m_l2l1_newEntry - tlb_l2l1.data(), 1, 0, 0, 0);
            }
            if (l2l2_newEntry) {
                TlbEntry *m_l2l2_newEntry =
                    lookup_l2tlb(vpnl2l2, asid, BaseMMU::Read, true, 2);
                if (m_l2l2_newEntry == NULL)
                    assert(0);
                l2TLB_remove(m_l2l2_newEntry - tlb_l2l2.data(), 0, 1, 0, 0);
            }
            if (l2l3_newEntry) {
                TlbEntry *m_l2l3_newEntry =
                    lookup_l2tlb(vpnl2l3, asid, BaseMMU::Read, true, 3);
                if (m_l2l3_newEntry == NULL)
                    assert(0);
                l2TLB_remove(m_l2l3_newEntry - tlb_l2l3.data(), 0, 0, 1, 0);
            }
            if (l2sp1_newEntry) {
                TlbEntry *m_l2sp1_newEntry =
                    lookup_l2tlb(vpnl2sp1, asid, BaseMMU::Read, true, 4);
                if (m_l2sp1_newEntry == NULL)
                    assert(0);
                l2TLB_remove(m_l2sp1_newEntry - tlb_l2sp.data(), 0, 0, 0, 1);
            }
            if (l2sp2_newEntry) {
                TlbEntry *m_l2sp2_newEntry =
                    lookup_l2tlb(vpnl2sp2, asid, BaseMMU::Read, true, 5);
                if (m_l2sp2_newEntry == NULL)
                    assert(0);
                l2TLB_remove(m_l2sp2_newEntry - tlb_l2sp.data(), 0, 0, 0, 1);
            }
        } else {
            for (i = 0; i < size; i++) {
                if (tlb[i].trieHandle) {
                    Addr mask = ~(tlb[i].size() - 1);
                    if ((vpn == 0 || (vpn & mask) == tlb[i].vaddr) &&
                        (asid == 0 || tlb[i].asid == asid))
                        remove(i);
                }
            }
            for (i = 0; i < l2tlb_l1_size * 8; i = i + 8) {
                if (tlb_l2l1[i].trieHandle){
                    Addr l2l1_mask = ~(tlb_l2l1[i].size() - 1);
                    if ((vpnl2l1 == 0 ||
                         (vpnl2l1 & l2l1_mask) == tlb_l2l1[i].vaddr) &&
                        (asid == 0 || tlb_l2l1[i].asid == asid))
                        l2TLB_remove(i, 1, 0, 0, 0);
                }
            }
            for (i = 0; i < l2tlb_l2_size * 8; i = i + 8) {
                if (tlb_l2l2[i].trieHandle){
                    Addr l2l2_mask = ~(tlb_l2l2[i].size() - 1);
                    if ((vpnl2l1 == 0 ||
                         (vpnl2l1 & l2l2_mask) == tlb_l2l2[i].vaddr) &&
                        (asid == 0 || tlb_l2l2[i].asid == asid))
                        l2TLB_remove(i, 0, 1, 0, 0);
                }
            }
            for (i = 0; i < l2tlb_l3_size * 8; i = i + 8) {
                if (tlb_l2l3[i].trieHandle){
                    Addr l2l3_mask = ~(tlb_l2l3[i].size() - 1);
                    if ((vpnl2l3 == 0 ||
                         (vpnl2l3 & l2l3_mask) == tlb_l2l3[i].vaddr) &&
                        (asid == 0 || tlb_l2l3[i].asid == asid))
                        l2TLB_remove(i, 0, 0, 1, 0);
                }
            }
            for (i = 0; i < l2tlb_sp_size * 8; i++) {
                Addr l2sp_mask = ~(tlb_l2sp[i].size() - 1);
                if (tlb_l2sp[i].trieHandle){

                    if ((vpnl2l1 == 0 ||
                         (vpnl2l1 & l2sp_mask) == tlb_l2sp[i].vaddr) &&
                        (asid == 0 || tlb_l2sp[i].asid == asid))
                        l2TLB_remove(i, 0, 0, 0, 1);
                }
                if (tlb_l2sp[i].trieHandle){
                    if ((vpnl2l2 == 0 ||
                         (vpnl2l2 & l2sp_mask) == tlb_l2sp[i].vaddr) &&
                        (asid == 0 || tlb_l2sp[i].asid == asid))
                        l2TLB_remove(i, 0, 0, 0, 1);
                }

            }
        }
    }
}

void
TLB::flushAll()
{
    DPRINTF(TLB, "flushAll()\n");
    size_t i;
    for (i = 0; i < size; i++) {
        if (tlb[i].trieHandle)
            remove(i);
    }

    for (i = 0; i < l2tlb_l1_size * 8; i = i + 8) {
        if (tlb_l2l1[i].trieHandle)
            l2TLB_remove(i, 1, 0, 0, 0);
    }

    for (i = 0; i < l2tlb_l2_size * 8; i = i + 8) {
        if (tlb_l2l2[i].trieHandle)
            l2TLB_remove(i, 0, 1, 0, 0);
    }
    for (i = 0; i < l2tlb_l3_size * 8; i = i + 8) {
        if (tlb_l2l3[i].trieHandle)
            l2TLB_remove(i, 0, 0, 1, 0);
    }
    for (i = 0; i < l2tlb_sp_size * 8; i = i + 8) {
        if (tlb_l2sp[i].trieHandle)
            l2TLB_remove(i, 0, 0, 0, 1);
    }
}

void
TLB::remove(size_t idx)
{
    DPRINTF(TLB, "remove(vpn=%#x, asid=%#x): ppn=%#x pte=%#x size=%#x\n",
        tlb[idx].vaddr, tlb[idx].asid, tlb[idx].paddr, tlb[idx].pte,
        tlb[idx].size());

    assert(tlb[idx].trieHandle);
    trie.remove(tlb[idx].trieHandle);
    tlb[idx].trieHandle = NULL;
    freeList.push_back(&tlb[idx]);
}

void
TLB::l2TLB_remove(size_t idx, int l2l1, int l2l2, int l2l3, int l2sp)
{
    int i;
    if (l2l1 == 1) {
        for (i = 0; i < 8; i++) {
            DPRINTF(TLB, "remove tlb_l2l1 idx %d idx+i %d\n", idx, idx + i);
            DPRINTF(TLB,
                    "remove tlb_l2l1 (vpn=%#x, asid=%#x): ppn=%#x pte=%#x "
                    "size=%#x\n",
                    tlb_l2l1[idx + i].vaddr, tlb_l2l1[idx + i].asid,
                    tlb_l2l1[idx + i].paddr, tlb_l2l1[idx + i].pte,
                    tlb_l2l1[idx + i].size());
            assert(tlb_l2l1[idx + i].trieHandle);
            trie_l2l1.remove(tlb_l2l1[idx + i].trieHandle);
            tlb_l2l1[idx + i].trieHandle = NULL;
            freeList_l2l1.push_back(&tlb_l2l1[idx + i]);
        }
    }
    if (l2l2 == 1) {
        for (i = 0; i < 8; i++) {
            DPRINTF(TLB, "remove tlb_l2l2 idx %d idx+i %d\n", idx, idx + i);
            DPRINTF(TLB,
                    "remove tlb_l2l2 (vpn=%#x, asid=%#x): ppn=%#x pte=%#x "
                    "size=%#x\n",
                    tlb_l2l2[idx + i].vaddr, tlb_l2l2[idx + i].asid,
                    tlb_l2l2[idx + i].paddr, tlb_l2l2[idx + i].pte,
                    tlb_l2l2[idx + i].size());
            assert(tlb_l2l2[idx + i].trieHandle);
            trie_l2l2.remove(tlb_l2l2[idx + i].trieHandle);
            tlb_l2l2[idx + i].trieHandle = NULL;
            freeList_l2l2.push_back(&tlb_l2l2[idx + i]);
        }
    }
    if (l2l3 == 1) {
        for (i = 0; i < 8; i++) {
            DPRINTF(TLB, "remove tlb_l2l3 idx %d idx+i %d\n", idx, idx + i);
            DPRINTF(TLB,
                    "remove tlb_l2l3 (vpn=%#x, asid=%#x): ppn=%#x pte=%#x "
                    "size=%#x\n",
                    tlb_l2l3[idx + i].vaddr, tlb_l2l3[idx + i].asid,
                    tlb_l2l3[idx + i].paddr, tlb_l2l3[idx + i].pte,
                    tlb_l2l3[idx + i].size());
            assert(tlb_l2l3[idx + i].trieHandle);
            trie_l2l3.remove(tlb_l2l3[idx + i].trieHandle);
            tlb_l2l3[idx + i].trieHandle = NULL;
            freeList_l2l3.push_back(&tlb_l2l3[idx + i]);
        }
    }
    if (l2sp == 1) {
        for (i = 0; i < 8; i++) {
            DPRINTF(TLB, "remove tlb_l2sp idx %d idx+i %d\n", idx, idx + i);
            DPRINTF(TLB,
                    "remove tlb_sp (vpn=%#x, asid=%#x): ppn=%#x pte=%#x "
                    "size=%#x\n",
                    tlb_l2sp[idx + i].vaddr, tlb_l2sp[idx + i].asid,
                    tlb_l2sp[idx + i].paddr, tlb_l2sp[idx + i].pte,
                    tlb_l2sp[idx + i].size());
            assert(tlb_l2sp[idx + i].trieHandle);
            trie_l2sp.remove(tlb_l2sp[idx + i].trieHandle);
            tlb_l2sp[idx + i].trieHandle = NULL;
            freeList_l2sp.push_back(&tlb_l2sp[idx + i]);
        }
    }
}

Fault
TLB::checkPermissions(STATUS status, PrivilegeMode pmode, Addr vaddr,
                      BaseMMU::Mode mode, PTESv39 pte)
{
    Fault fault = NoFault;

    if (mode == BaseMMU::Read && !pte.r) {
        DPRINTF(TLB, "PTE has no read perm, raising PF\n");
        fault = createPagefault(vaddr, mode);
    }
    else if (mode == BaseMMU::Write && !pte.w) {
        DPRINTF(TLB, "PTE has no write perm, raising PF\n");
        fault = createPagefault(vaddr, mode);
    }
    else if (mode == BaseMMU::Execute && !pte.x) {
        DPRINTF(TLB, "PTE has no exec perm, raising PF\n");
        fault = createPagefault(vaddr, mode);
    }

    if (fault == NoFault) {
        // check pte.u
        if (pmode == PrivilegeMode::PRV_U && !pte.u) {
            DPRINTF(TLB, "PTE is not user accessible, raising PF\n");
            fault = createPagefault(vaddr, mode);
        }
        else if (pmode == PrivilegeMode::PRV_S && pte.u && status.sum == 0) {
            DPRINTF(TLB, "PTE is only user accessible, raising PF\n");
            fault = createPagefault(vaddr, mode);
        }
    }

    return fault;
}

Fault
TLB::createPagefault(Addr vaddr, BaseMMU::Mode mode)
{
    ExceptionCode code;
    if (mode == BaseMMU::Read)
        code = ExceptionCode::LOAD_PAGE;
    else if (mode == BaseMMU::Write)
        code = ExceptionCode::STORE_PAGE;
    else
        code = ExceptionCode::INST_PAGE;
    DPRINTF(TLB, "Create page fault #%i on %#lx\n", code, vaddr);
    return std::make_shared<AddressFault>(vaddr, code);
}

Addr
TLB::translateWithTLB(Addr vaddr, uint16_t asid, BaseMMU::Mode mode)
{
    TlbEntry *e = lookup(vaddr, asid, mode, false);
    DPRINTF(TLB,"translateWithTLB vaddr %#x \n",vaddr);
    assert(e != nullptr);
    DPRINTF(TLB,"translateWithTLB vaddr %#x paddr %#x\n",
            vaddr,e->paddr << PageShift | (vaddr & mask(e->logBytes)));
    return e->paddr << PageShift | (vaddr & mask(e->logBytes));
}
Fault
TLB::L2tlb_pagefault(Addr vaddr, BaseMMU::Mode mode, const RequestPtr &req)
{
    if (req->isInstFetch()) {
        Addr page_l2_start = (vaddr >> 12) << 12;
        DPRINTF(TLBVerbosel2, "vaddr %#x,req_pc %#x,page_l2_start %#x\n",
                vaddr, req->getPC(), page_l2_start);
        if (req->getPC() < page_l2_start) {
            DPRINTF(TLBVerbosel2, "vaddr %#x,req_pc %#x,page_l2_start %#x\n",
                    vaddr, req->getPC(), page_l2_start);
            return createPagefault(page_l2_start, mode);
        }
        return createPagefault(req->getPC(), mode);
    } else {
        DPRINTF(TLBVerbosel2, "vaddr 2 %#x,req_pc %#x,get vaddr %#x\n", vaddr,
                req->getPC(), req->getVaddr());
        return createPagefault(req->getVaddr(), mode);
    }
}

Fault
TLB::L2tlb_check(PTESv39 pte, int level, STATUS status, PrivilegeMode pmode,
                 Addr vaddr, BaseMMU::Mode mode, const RequestPtr &req,
                 ThreadContext *tc, BaseMMU::Translation *translation)
{
    Fault fault = NoFault;
    hit_in_sp = false;
    DPRINTF(TLB, "l2tlb_check paddr %#x vaddr %#x pte %#x\n", pte.ppn, vaddr,
            pte);
    DPRINTF(TLB, "pte %#x r %d x %d \n", pte, pte.r, pte.x);
    //
    if (!pte.v || (!pte.r && pte.w)) {
        hit_in_sp = true;
        DPRINTF(TLB, "check l2 tlb PTE invalid, raising PF\n");
        fault = L2tlb_pagefault(vaddr, mode, req);

    } else {
        if (pte.r || pte.x) {
            hit_in_sp = true;
            fault = checkPermissions(status, pmode, vaddr, mode, pte);
            if (fault == NoFault) {
                if (level >= 1 && pte.ppn0 != 0) {
                    DPRINTF(TLB, "L2TLB PTE has misaligned PPN, raising PF\n");
                    fault = L2tlb_pagefault(vaddr, mode, req);
                } else if (level == 2 && pte.ppn1 != 0) {
                    DPRINTF(TLB, "L2TLB PTE has misaligned PPN, raising PF\n");
                    fault = L2tlb_pagefault(vaddr, mode, req);
                }
            }

            if (fault == NoFault) {
                if (!pte.a) {
                    DPRINTF(TLB, "L2TLB PTE needs to write pte.a\n");
                    fault = L2tlb_pagefault(vaddr, mode, req);
                }
                if (!pte.d && mode == BaseMMU::Write) {
                    DPRINTF(TLB, "L2TLB PTE needs to write pte.d\n");
                    fault = L2tlb_pagefault(vaddr, mode, req);
                }
            }


        } else {
            level--;
            if (level < 0) {
                hit_in_sp = true;
                DPRINTF(TLB, "No leaf PTE found raising PF\n");
                fault = L2tlb_pagefault(vaddr, mode, req);
            } else {
                hit_in_sp = false;
                fault = walker->start(pte.ppn, tc, translation, req, mode,
                                      false, level, true);
            }
        }
    }
    DPRINTF(TLB, "tlb check final\n");
    if (fault == NoFault)
        DPRINTF(TLB, "the result is nofault\n");
    else
        DPRINTF(TLB, "the result is fault for some reason\n");
    return fault;
}

Fault
TLB::doTranslate(const RequestPtr &req, ThreadContext *tc,
                 BaseMMU::Translation *translation, BaseMMU::Mode mode,
                 bool &delayed)
{
    delayed = false;

    Addr vaddr = Addr(sext<VADDR_BITS>(req->getVaddr()));
    SATP satp = tc->readMiscReg(MISCREG_SATP);

    TlbEntry *e = lookup(vaddr, satp.asid, mode, false);
    TlbEntry *e5 = NULL;
    TlbEntry *e4 = NULL;
    TlbEntry *e3 = NULL;
    TlbEntry *e2 = NULL;
    TlbEntry *e1 = NULL;
    Addr paddr;
    Fault fault;
    // e5->pte.v;
    STATUS status = tc->readMiscReg(MISCREG_STATUS);
    PrivilegeMode pmode = getMemPriv(tc, mode);
    DPRINTF(TLB, "the original vaddr %#x\n", vaddr);

    if (!e) {  // look up l2tlb
        e5 = lookup_l2tlb(vaddr, satp.asid, mode, false, 5);
        e4 = lookup_l2tlb(vaddr, satp.asid, mode, false, 4);
        e3 = lookup_l2tlb(vaddr, satp.asid, mode, false, 3);
        e2 = lookup_l2tlb(vaddr, satp.asid, mode, false, 2);
        e1 = lookup_l2tlb(vaddr, satp.asid, mode, false, 1);
        if (e3) {  // if hit in l3tlb
            DPRINTF(TLB, "hit in l2TLB l3\n");
            fault = L2tlb_check(e3->pte, 0, status, pmode, vaddr, mode, req,
                                tc, translation);
            e = e3;

            if (hit_in_sp) {
                e = e3;
                if (fault == NoFault) {
                    paddr =
                        e->paddr << PageShift | (vaddr & mask(e->logBytes));
                    insert(e->vaddr, *e);
                    walker->doL2TLBHitSchedule(req, tc, translation, mode,
                                               paddr);
                    delayed = true;
                    return fault;
                }
                else
                    return fault;

            } else {
                if (translation != nullptr || fault != NoFault) {
                    // This gets ignored in atomic mode.
                    delayed = true;
                    return fault;
                }
                e = lookup(vaddr, satp.asid, mode, false);
                assert(e != nullptr);
            }
        } else if (e5) {  // hit in sp l2
            DPRINTF(TLB, "hit in l2 tlb l5\n");
            fault = L2tlb_check(e5->pte, 1, status, pmode, vaddr, mode, req,
                                tc, translation);
            if (hit_in_sp){
                e = e5;
                if (fault == NoFault) {
                    paddr =
                        e->paddr << PageShift | (vaddr & mask(e->logBytes));
                    insert(e->vaddr, *e);
                    walker->doL2TLBHitSchedule(req, tc, translation, mode,
                                               paddr);
                    delayed = true;
                    return fault;
                }
                else
                    return fault;
            }
            else {
                if (translation != nullptr || fault != NoFault) {
                    // This gets ignored in atomic mode.
                    delayed = true;
                    return fault;
                }
                e = lookup(vaddr, satp.asid, mode, false);
                assert(e != nullptr);
            }

        } else if (e4) {  // hit in sp l1
            DPRINTF(TLB, "hit in l2 tlb l4\n");
            fault = L2tlb_check(e4->pte, 2, status, pmode, vaddr, mode, req,
                                tc, translation);
            if (hit_in_sp){
                e =  e4;
                if (fault == NoFault) {
                    paddr =
                        e->paddr << PageShift | (vaddr & mask(e->logBytes));
                    insert(e->vaddr, *e);
                    walker->doL2TLBHitSchedule(req, tc, translation, mode,
                                               paddr);
                    delayed = true;
                    return fault;
                }
                else
                    return fault;
            }
            else{
                if (translation != nullptr || fault != NoFault){
                    delayed = true;
                    return fault;
                }
                e = lookup(vaddr,satp.asid,mode,false) ;
                assert(e != nullptr);
            }
            //e = e4;
        } else if (e2) {
            DPRINTF(TLB, "hit in l2 tlb l2\n");
            fault = L2tlb_check(e2->pte, 1, status, pmode, vaddr, mode, req,
                                tc, translation);
            if (hit_in_sp){
                e = e2;
                if (fault == NoFault) {
                    paddr =
                        e->paddr << PageShift | (vaddr & mask(e->logBytes));
                    insert(e->vaddr, *e);
                    walker->doL2TLBHitSchedule(req, tc, translation, mode,
                                               paddr);
                    delayed = true;
                    return fault;
                }
                else
                    return fault;
            }
            else{
                if (translation != nullptr || fault != NoFault) {
                    // This gets ignored in atomic mode.
                    delayed = true;
                    return fault;
                }
                e = lookup(vaddr, satp.asid, mode, false);
                assert(e != nullptr);
            }


        } else if (e1) {
            DPRINTF(TLB, "hit in l2 tlb l1\n");
            fault = L2tlb_check(e1->pte, 2, status, pmode, vaddr, mode, req,
                                tc, translation);
            if (hit_in_sp){
                e = e1;
                if (fault == NoFault) {
                    paddr =
                        e->paddr << PageShift | (vaddr & mask(e->logBytes));
                    insert(e->vaddr, *e);
                    walker->doL2TLBHitSchedule(req, tc, translation, mode,
                                               paddr);
                    delayed = true;
                    return fault;
                }
                else
                 return fault;
            }
            else{
                if (translation != nullptr || fault != NoFault) {
                    // This gets ignored in atomic mode.
                    delayed = true;
                    return fault;
                }
                e = lookup(vaddr, satp.asid, mode, false);
                assert(e != nullptr);
            }

        } else {
            DPRINTF(TLB, "miss in l1 tlb + l2 tlb\n");
            fault = walker->start(0,tc, translation, req, mode, false);
            DPRINTF(TLB,"finish start\n");
            if (translation != nullptr || fault != NoFault) {
                // This gets ignored in atomic mode.
                delayed = true;
                return fault;
            }
            e = lookup(vaddr, satp.asid, mode, false);
            assert(e != nullptr);
        }
    }

    status = tc->readMiscReg(MISCREG_STATUS);
    pmode = getMemPriv(tc, mode);
    if (fault == NoFault) {
        fault = checkPermissions(status, pmode, vaddr, mode, e->pte);
    }

    if (fault != NoFault) {
        // if we want to write and it isn't writable, do a page table walk
        // again to update the dirty flag.
        DPRINTF(TLB,"check is no fault\n");
        if (mode == BaseMMU::Write && !e->pte.w) {
            DPRINTF(TLB, "Dirty bit not set, repeating PT walk\n");
            fault = walker->start(0, tc, translation, req, mode);
            if (translation != nullptr || fault != NoFault) {
                delayed = true;
                return fault;
            }
        }
        if (fault != NoFault)
            return fault;
    }
    assert(e != nullptr);
    paddr = e->paddr << PageShift | (vaddr & mask(e->logBytes));

    DPRINTF(TLBVerbose, "translate(vpn=%#x, asid=%#x): %#x\n",
            vaddr, satp.asid, paddr);
    req->setPaddr(paddr);

    return NoFault;
}

PrivilegeMode
TLB::getMemPriv(ThreadContext *tc, BaseMMU::Mode mode)
{
    STATUS status = (STATUS)tc->readMiscReg(MISCREG_STATUS);
    PrivilegeMode pmode = (PrivilegeMode)tc->readMiscReg(MISCREG_PRV);
    if (mode != BaseMMU::Execute && status.mprv == 1)
        pmode = (PrivilegeMode)(RegVal)status.mpp;
    return pmode;
}

Fault
TLB::translate(const RequestPtr &req, ThreadContext *tc,
               BaseMMU::Translation *translation, BaseMMU::Mode mode,
               bool &delayed)
{
    delayed = false;

    if (FullSystem) {
        PrivilegeMode pmode = getMemPriv(tc, mode);
        SATP satp = tc->readMiscReg(MISCREG_SATP);
        if (pmode == PrivilegeMode::PRV_M || satp.mode == AddrXlateMode::BARE)
            req->setFlags(Request::PHYSICAL);

        Fault fault;
        if (req->getFlags() & Request::PHYSICAL) {
            /**
             * we simply set the virtual address to physical address
             */
            req->setPaddr(req->getVaddr());
            fault = NoFault;
        } else {
            fault = doTranslate(req, tc, translation, mode, delayed);
        }

        // according to the RISC-V tests, negative physical addresses trigger
        // an illegal address exception.
        // TODO where is that written in the manual?
        if (!delayed && fault == NoFault && bits(req->getPaddr(), 63)) {
            ExceptionCode code;
            if (mode == BaseMMU::Read)
                code = ExceptionCode::LOAD_ACCESS;
            else if (mode == BaseMMU::Write)
                code = ExceptionCode::STORE_ACCESS;
            else
                code = ExceptionCode::INST_ACCESS;
            fault = std::make_shared<AddressFault>(req->getVaddr(), code);
        }

        if (!delayed && fault == NoFault) {
            pma->check(req);

            // do pmp check if any checking condition is met.
            // mainFault will be NoFault if pmp checks are
            // passed, otherwise an address fault will be returned.
            fault = pmp->pmpCheck(req, mode, pmode, tc);
        }
       // printf("before translate fault\n");

        return fault;
    } else {
        // In the O3 CPU model, sometimes a memory access will be speculatively
        // executed along a branch that will end up not being taken where the
        // address is invalid.  In that case, return a fault rather than trying
        // to translate it (which will cause a panic).  Since RISC-V allows
        // unaligned memory accesses, this should only happen if the request's
        // length is long enough to wrap around from the end of the memory to
        // the start.
        assert(req->getSize() > 0);
        if (req->getVaddr() + req->getSize() - 1 < req->getVaddr())
            return std::make_shared<GenericPageTableFault>(req->getVaddr());

        Process * p = tc->getProcessPtr();

        Fault fault = p->pTable->translate(req);
        if (fault != NoFault)
            return fault;

        return NoFault;
    }
}

Fault
TLB::translateAtomic(const RequestPtr &req, ThreadContext *tc,
                     BaseMMU::Mode mode)
{
    bool delayed;
    return translate(req, tc, nullptr, mode, delayed);
}

void
TLB::translateTiming(const RequestPtr &req, ThreadContext *tc,
                     BaseMMU::Translation *translation, BaseMMU::Mode mode)
{
    bool delayed;
    assert(translation);
    Fault fault = translate(req, tc, translation, mode, delayed);
    if (!delayed){
       // printf("!delayed \n");
        translation->finish(fault, req, tc, mode);

    }

    else
        translation->markDelayed();
}

Fault
TLB::translateFunctional(const RequestPtr &req, ThreadContext *tc,
                         BaseMMU::Mode mode)
{
    const Addr vaddr = req->getVaddr();
    Addr paddr = vaddr;

    if (FullSystem) {
        MMU *mmu = static_cast<MMU *>(tc->getMMUPtr());

        PrivilegeMode pmode = mmu->getMemPriv(tc, mode);
        SATP satp = tc->readMiscReg(MISCREG_SATP);
        if (pmode != PrivilegeMode::PRV_M &&
            satp.mode != AddrXlateMode::BARE) {
            Walker *walker = mmu->getDataWalker();
            unsigned logBytes;
            Fault fault = walker->startFunctional(
                    tc, paddr, logBytes, mode);
            if (fault != NoFault)
                return fault;

            Addr masked_addr = vaddr & mask(logBytes);
            paddr |= masked_addr;
        }
    }
    else {
        Process *process = tc->getProcessPtr();
        const auto *pte = process->pTable->lookup(vaddr);

        if (!pte && mode != BaseMMU::Execute) {
            // Check if we just need to grow the stack.
            if (process->fixupFault(vaddr)) {
                // If we did, lookup the entry for the new page.
                pte = process->pTable->lookup(vaddr);
            }
        }

        if (!pte)
            return std::make_shared<GenericPageTableFault>(req->getVaddr());

        paddr = pte->paddr | process->pTable->pageOffset(vaddr);
    }

    DPRINTF(TLB, "Translated (functional) %#x -> %#x.\n", vaddr, paddr);
    req->setPaddr(paddr);
    return NoFault;
}

Fault
TLB::finalizePhysical(const RequestPtr &req,
                      ThreadContext *tc, BaseMMU::Mode mode) const
{
    return NoFault;
}

void
TLB::serialize(CheckpointOut &cp) const
{
    // Only store the entries in use.
    printf("serialize\n");
    uint32_t _size = size - freeList.size();
    SERIALIZE_SCALAR(_size);
    SERIALIZE_SCALAR(lruSeq);

    uint32_t _count = 0;
    for (uint32_t x = 0; x < size; x++) {
        if (tlb[x].trieHandle != NULL)
            tlb[x].serializeSection(cp, csprintf("Entry%d", _count++));
    }
}

void
TLB::unserialize(CheckpointIn &cp)
{
    // Do not allow to restore with a smaller tlb.
    printf("unserialize\n");
    uint32_t _size;
    UNSERIALIZE_SCALAR(_size);
    if (_size > size) {
        fatal("TLB size less than the one in checkpoint!");
    }

    UNSERIALIZE_SCALAR(lruSeq);

    for (uint32_t x = 0; x < _size; x++) {
        TlbEntry *newEntry = freeList.front();
        freeList.pop_front();

        newEntry->unserializeSection(cp, csprintf("Entry%d", x));
        Addr key = buildKey(newEntry->vaddr, newEntry->asid);
        newEntry->trieHandle = trie.insert(key,
            TlbEntryTrie::MaxBits - newEntry->logBytes, newEntry);
    }
}

TLB::TlbStats::TlbStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(readHits, statistics::units::Count::get(), "read hits"),
      ADD_STAT(readMisses, statistics::units::Count::get(), "read misses"),
      ADD_STAT(readAccesses, statistics::units::Count::get(), "read accesses"),
      ADD_STAT(writeHits, statistics::units::Count::get(), "write hits"),
      ADD_STAT(writeMisses, statistics::units::Count::get(), "write misses"),
      ADD_STAT(writeAccesses, statistics::units::Count::get(),
               "write accesses"),
      ADD_STAT(readprefetchHits, statistics::units::Count::get(),
               "read prefetch Hits"),
      ADD_STAT(writeprefetchHits, statistics::units::Count::get(),
               "write prefetch Hits"),
      ADD_STAT(readprefetchAccesses, statistics::units::Count::get(),
               "read prefetch Accesses"),
      ADD_STAT(writeprefetchAccesses, statistics::units::Count::get(),
               "write prefetch Accesses"),
      ADD_STAT(readprefetchMisses, statistics::units::Count::get(),
               "read prefetch Misses"),
      ADD_STAT(writeprefetchMisses, statistics::units::Count::get(),
               "write prefetch Misses"),
      ADD_STAT(hits, statistics::units::Count::get(),
               "Total TLB (read and write) hits", readHits + writeHits),
      ADD_STAT(misses, statistics::units::Count::get(),
               "Total TLB (read and write) misses", readMisses + writeMisses),
      ADD_STAT(accesses, statistics::units::Count::get(),
               "Total TLB (read and write) accesses",
               readAccesses + writeAccesses)
{
}

Port *
TLB::getTableWalkerPort()
{
    return &walker->getPort("port");
}

} // namespace gem5
