/**
 * Copyright (c) 2018 Metempsy Technology Consulting
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

#include "mem/cache/prefetch/cdp.hh"

#include <queue>

#include "debug/CDPUseful.hh"
#include "debug/CDPdebug.hh"
#include "debug/CDPdepth.hh"
#include "debug/HWPrefetch.hh"
#include "debug/WorkerPref.hh"
#include "mem/cache/base.hh"
#include "mem/packet.hh"
#include "mem/packet_access.hh"
#include "params/CDP.hh"
#include "sim/byteswap.hh"

// similar to x[hi:lo] in verilog

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{
CDP::CDP(const CDPParams &p) : Queued(p), depth_threshold(3), byteOrder(p.sys->getGuestByteOrder()),
                               cdpStats(this)
{
    for (int i = 0; i < PrefetchSourceType::NUM_PF_SOURCES; i++) {
        enable_prf_filter.push_back(false);
    }
}

CDP::CDPStats::CDPStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(triggeredInRxNotify, statistics::units::Count::get(),
               "Number of times the prefetcher was triggered in rxNotify"),
      ADD_STAT(triggeredInCalcPf, statistics::units::Count::get(),
               "Number of times the prefetcher was triggered in calculatePrefetch"),
      ADD_STAT(hitNotifyCalled, statistics::units::Count::get(),
               "Number of times the prefetcher was called in hitNotify"),
      ADD_STAT(hitNotifyExitBlockNotFound, statistics::units::Count::get(),
               "Number of times the prefetcher exited hitNotify due to block not found"),
      ADD_STAT(hitNotifyExitFilter, statistics::units::Count::get(),
               "Number of times the prefetcher exited hitNotify due to filter"),
      ADD_STAT(hitNotifyExitDepth, statistics::units::Count::get(),
               "Number of times the prefetcher exited hitNotify due to depth"),
      ADD_STAT(hitNotifyNoAddrFound, statistics::units::Count::get(),
               "Number of times the prefetcher exited hitNotify due to no address found"),
      ADD_STAT(hitNotifyNoVA, statistics::units::Count::get(),
               "Number of times the prefetcher exited hitNotify due to no VA"),
      ADD_STAT(hitNotifyNoData, statistics::units::Count::get(),
               "Number of times the prefetcher exited hitNotify due to no data"),
      ADD_STAT(missNotifyCalled, statistics::units::Count::get(),
               "Number of times the prefetcher was called in missNotify")
{
}

void
CDP::calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses)
{
    Addr addr = pfi.getAddr();
    bool miss = pfi.isCacheMiss();
    int page_offset, vpn0, vpn1, vpn2;
    PrefetchSourceType pf_source = pfi.getXsMetadata().prefetchSource;
    int pf_depth = pfi.getXsMetadata().prefetchDepth;
    if (!miss && pfi.getDataPtr() != nullptr) {
        size_t found = cache->system->getRequestorName(pfi.getRequestorId()).find("dcache.prefetcher");
        if (found != std::string::npos) {
            if (enable_prf_filter[pf_source])
                return;
        }
        DPRINTF(CDPdepth, "HIT Depth: %d\n", pfi.getXsMetadata().prefetchDepth);
        if (((pf_depth == 4 || pf_depth == 2))) {
            uint64_t *test_addrs = pfi.getDataPtr();
            std::queue<std::pair<CacheBlk *, Addr>> pt_blks;
            std::vector<uint64_t> addrs;
            switch (byteOrder) {
                case ByteOrder::big:
                    for (int of = 0; of < 8; of++) {
                        addrs.push_back(Addr(betoh(*(uint64_t *)(test_addrs + of))));
                    }
                    break;

                case ByteOrder::little:
                    for (int of = 0; of < 8; of++) {
                        addrs.push_back(Addr(letoh(*(uint64_t *)(test_addrs + of))));
                    }
                    break;

                default:
                    panic(
                        "Illegal byte order in \
                            CDP::notifyFill(const PacketPtr &pkt)\n");
            }
            for (Addr pt_addr : scanPointer(addr, addrs)) {
                AddrPriority addrprio = AddrPriority(pt_addr, 30, PrefetchSourceType::CDP);
                addrprio.depth = 1;
                // addresses.push_back(addrprio);
                sendPFWithFilter(pt_addr, addresses, 30, PrefetchSourceType::CDP, 1);
                cdpStats.triggeredInCalcPf++;
            }
        }
    } else if (miss) {
        cdpStats.missNotifyCalled++;
        DPRINTF(CDPUseful, "Miss addr: %#llx\n", addr);
        vpn2 = BITS(addr, 38, 30);
        vpn1 = BITS(addr, 29, 21);
        vpn0 = BITS(addr, 20, 12);
        page_offset = BITS(addr, 11, 0);
        vpnTable.add(vpn2, vpn1);
        vpnTable.resetConfidence();
        DPRINTF(CDPdebug,
                "Sv39, ADDR:%#llx, vpn2:%#llx, vpn1:%#llx, vpn0:%#llx, page offset:%#llx\n",
                addr, Addr(vpn2), Addr(vpn1), Addr(vpn0), Addr(page_offset));
    }
    return;
}

void
CDP::notifyFill(const PacketPtr &pkt)
{
    cdpStats.hitNotifyCalled++;
    assert(pkt);
    assert(cache);
    uint64_t test_addr = 0;
    std::vector<uint64_t> addrs;
    if (pkt->hasData() && pkt->req->hasVaddr()) {
        DPRINTF(CDPdebug, "Hit notify received for addr: %#llx\n", pkt->req->getVaddr());
        if (cache->findBlock(pkt->getAddr(), pkt->isSecure()) == nullptr) {
            cdpStats.hitNotifyExitBlockNotFound++;
            return;
        }
        Request::XsMetadata pkt_meta = cache->getHitBlkXsMetadata(pkt);
        size_t found = cache->system->getRequestorName(pkt->req->requestorId()).find("dcache.prefetcher");
        int pf_depth = pkt_meta.prefetchDepth;
        PrefetchSourceType pf_source = pkt_meta.prefetchSource;
        if (found != std::string::npos) {
            if (enable_prf_filter[pkt->req->getXsMetadata().prefetchSource]) {
                cdpStats.hitNotifyExitFilter++;
                return;
            }
        }
        uint64_t *test_addr_start = (uint64_t *)pkt->getPtr<uint64_t>();
        unsigned max_offset = pkt->getSize() / 8;
        switch (byteOrder) {
            case ByteOrder::big:
                for (unsigned of = 0; of < max_offset; of++) {
                    addrs.push_back(Addr(betoh(*(uint64_t *)(test_addr_start + of))));
                }
                break;

            case ByteOrder::little:
                for (unsigned of = 0; of < max_offset; of++) {
                    addrs.push_back(Addr(letoh(*(uint64_t *)(test_addr_start + of))));
                }
                break;

            default:
                panic(
                    "Illegal byte order in \
                        CDP::notifyFill(const PacketPtr &pkt)\n");
        };

        unsigned sentCount = 0;
        std::vector<AddrPriority> addresses;
        for (int of = 0; of < max_offset; of++) {
            test_addr = addrs[of];
            int align_bit = BITS(test_addr, 1, 0);
            int filter_bit = BITS(test_addr, 5, 0);
            int page_offset, vpn0, vpn1, vpn1_addr, vpn2, vpn2_addr, check_bit;
            check_bit = BITS(test_addr, 63, 39);
            vpn2 = BITS(test_addr, 38, 30);
            vpn2_addr = BITS(pkt->req->getVaddr(), 38, 30);
            vpn1 = BITS(test_addr, 29, 21);
            vpn1_addr = BITS(pkt->req->getVaddr(), 29, 21);
            vpn0 = BITS(test_addr, 20, 12);
            page_offset = BITS(test_addr, 11, 0);
            bool flag = true;
            if ((check_bit != 0) || (!vpnTable.search(vpn2, vpn1)) || (vpn0 == 0) || (align_bit != 0))
                flag = false;
            Addr test_addr2 = Addr(test_addr);
            if (flag) {
                if (pf_depth >= depth_threshold) {
                    cdpStats.hitNotifyExitDepth++;
                    return;
                }
                int next_depth = 0;
                if (pf_depth == 0) {
                    next_depth = 4;
                } else {
                    next_depth = pf_depth + 1;
                }
                AddrPriority addrprio =
                    AddrPriority(blockAddress(test_addr2), 29 + next_depth, PrefetchSourceType::CDP);
                addrprio.depth = next_depth;
                sendPFWithFilter(blockAddress(test_addr2), addresses, 29 + next_depth, PrefetchSourceType::CDP,
                                 next_depth);
                // addresses.push_back(addrprio);
                AddrPriority addrprio2 =
                    AddrPriority(blockAddress(test_addr2) + 0x40, 29 + next_depth - 10, PrefetchSourceType::CDP);
                addrprio2.depth = next_depth;
                // addresses.push_back(addrprio2);
                sendPFWithFilter(blockAddress(test_addr2) + 0x40, addresses, 29 + next_depth - 10,
                                 PrefetchSourceType::CDP, next_depth);
                cdpStats.triggeredInRxNotify++;
                sentCount++;
            }
        }
        if (sentCount == 0) {
            cdpStats.hitNotifyNoAddrFound++;
        }
        PrefetchInfo pfi(pkt, pkt->req->getVaddr(), false);
        size_t max_pfs = getMaxPermittedPrefetches(addresses.size());

        // Queue up generated prefetches
        size_t num_pfs = 0;
        for (AddrPriority &addr_prio : addresses) {

            // Block align prefetch address
            addr_prio.addr = blockAddress(addr_prio.addr);

            if (!samePage(addr_prio.addr, pfi.getAddr())) {
                statsQueued.pfSpanPage += 1;

                if (hasBeenPrefetched(pkt->getAddr(), pkt->isSecure())) {
                    statsQueued.pfUsefulSpanPage += 1;
                }
            }

            bool can_cross_page = (tlb != nullptr);
            if (can_cross_page || samePage(addr_prio.addr, pfi.getAddr())) {
                PrefetchInfo new_pfi(pfi, addr_prio.addr);
                new_pfi.setXsMetadata(Request::XsMetadata(addr_prio.pfSource, addr_prio.depth));
                statsQueued.pfIdentified++;
                DPRINTF(HWPrefetch,
                        "Found a pf candidate addr: %#x, "
                        "inserting into prefetch queue.\n",
                        new_pfi.getAddr());
                // Create and insert the request
                insert(pkt, new_pfi, addr_prio);
                num_pfs += 1;
                if (num_pfs == max_pfs) {
                    break;
                }
            } else {
                DPRINTF(HWPrefetch, "Ignoring page crossing prefetch.\n");
            }
        }
    }
    if (!pkt->req->hasVaddr()) cdpStats.hitNotifyNoVA++;
    if (!pkt->hasData()) cdpStats.hitNotifyNoData++;
    return;
}

void
CDP::pfHitNotify(float accuracy, PrefetchSourceType pf_source, const PacketPtr &pkt)
{
    if (accuracy < 0.1) {
        enable_prf_filter[pf_source] = true;
    } else {
        enable_prf_filter[pf_source] = false;
    }
    notifyFill(pkt);
}

bool
CDP::sendPFWithFilter(Addr addr, std::vector<AddrPriority> &addresses, int prio, PrefetchSourceType pfSource,
                      int pf_depth)
{
    if (pfLRUFilter->contains((addr))) {
        return false;
    } else {
        pfLRUFilter->insert((addr), 0);
        AddrPriority addrprio = AddrPriority(addr, prio, pfSource);
        addrprio.depth = pf_depth;
        addresses.push_back(addrprio);
        return true;
    }
    return false;
}

}  // namespace prefetch
}  // namespace gem5
