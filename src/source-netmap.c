/* Copyright (C) 2011-2014 Open Information Security Foundation
 *
 * You can copy, redistribute or modify this Program under the terms of
 * the GNU General Public License version 2 as published by the Free
 * Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

/**
*  \defgroup netmap Netmap running mode
*
*  @{
*/

/**
* \file
*
* \author Aleksey Katargin <gureedo@gmail.com>
*
* Netmap socket acquisition support
*
*/

#include "suricata-common.h"
#include "config.h"
#include "suricata.h"
#include "decode.h"
#include "packet-queue.h"
#include "threads.h"
#include "threadvars.h"
#include "tm-queuehandlers.h"
#include "tm-modules.h"
#include "tm-threads.h"
#include "tm-threads-common.h"
#include "conf.h"
#include "util-debug.h"
#include "util-device.h"
#include "util-error.h"
#include "util-privs.h"
#include "util-optimize.h"
#include "util-checksum.h"
#include "util-ioctl.h"
#include "util-host-info.h"
#include "tmqh-packetpool.h"
#include "source-netmap.h"
#include "runmodes.h"

#ifdef __SC_CUDA_SUPPORT__

#include "util-cuda.h"
#include "util-cuda-buffer.h"
#include "util-mpm-ac.h"
#include "util-cuda-handlers.h"
#include "detect-engine.h"
#include "detect-engine-mpm.h"
#include "util-cuda-vars.h"

#endif /* __SC_CUDA_SUPPORT__ */

#ifdef HAVE_NETMAP

#if HAVE_SYS_IOCTL_H
#include <sys/ioctl.h>
#endif

#if HAVE_SYS_MMAN_H
#include <sys/mman.h>
#endif

#include <net/netmap_user.h>

#endif /* HAVE_NETMAP */

extern int max_pending_packets;

#ifndef HAVE_NETMAP

TmEcode NoNetmapSupportExit(ThreadVars *, void *, void **);

void TmModuleReceiveNetmapRegister (void)
{
    tmm_modules[TMM_RECEIVENETMAP].name = "ReceiveNetmap";
    tmm_modules[TMM_RECEIVENETMAP].ThreadInit = NoNetmapSupportExit;
    tmm_modules[TMM_RECEIVENETMAP].Func = NULL;
    tmm_modules[TMM_RECEIVENETMAP].ThreadExitPrintStats = NULL;
    tmm_modules[TMM_RECEIVENETMAP].ThreadDeinit = NULL;
    tmm_modules[TMM_RECEIVENETMAP].RegisterTests = NULL;
    tmm_modules[TMM_RECEIVENETMAP].cap_flags = 0;
    tmm_modules[TMM_RECEIVENETMAP].flags = TM_FLAG_RECEIVE_TM;
}

/**
* \brief Registration Function for DecodeNetmap.
* \todo Unit tests are needed for this module.
*/
void TmModuleDecodeNetmapRegister (void)
{
    tmm_modules[TMM_DECODENETMAP].name = "DecodeNetmap";
    tmm_modules[TMM_DECODENETMAP].ThreadInit = NoNetmapSupportExit;
    tmm_modules[TMM_DECODENETMAP].Func = NULL;
    tmm_modules[TMM_DECODENETMAP].ThreadExitPrintStats = NULL;
    tmm_modules[TMM_DECODENETMAP].ThreadDeinit = NULL;
    tmm_modules[TMM_DECODENETMAP].RegisterTests = NULL;
    tmm_modules[TMM_DECODENETMAP].cap_flags = 0;
    tmm_modules[TMM_DECODENETMAP].flags = TM_FLAG_DECODE_TM;
}

/**
* \brief this function prints an error message and exits.
*/
TmEcode NoNetmapSupportExit(ThreadVars *tv, void *initdata, void **data)
{
    SCLogError(SC_ERR_NO_NETMAP,"Error creating thread %s: you do not have "
            "support for netmap enabled, please recompile "
            "with --enable-netmap", tv->name);
    exit(EXIT_FAILURE);
}

#else /* We have NETMAP support */

#define POLL_TIMEOUT 100

#if defined(__linux__)
#define POLL_EVENTS (POLLHUP|POLLRDHUP|POLLERR|POLLNVAL)
#else
#define POLL_EVENTS (POLLHUP|POLLERR|POLLNVAL)
#endif

enum {
    NETMAP_OK,
    NETMAP_FAILURE,
};

enum {
    NETMAP_FLAG_ZERO_COPY = 1,
};

/**
 * \brief Netmap ring isntance.
 */
typedef struct NetmapRing
{
    int fd;
    struct netmap_ring *rx;
    struct netmap_ring *tx;
    SCSpinlock tx_lock;
} NetmapRing;

/**
 * \brief Netmap device instance.
 */
typedef struct NetmapDevice_
{
    char ifname[IFNAMSIZ];
    void *mem;
    size_t memsize;
    struct netmap_if *nif;
    int rings_cnt;
    NetmapRing *rings;
    unsigned int ref;
    SC_ATOMIC_DECLARE(unsigned int, threads_run);
    TAILQ_ENTRY(NetmapDevice_) next;
} NetmapDevice;

/**
 * \brief Module thread local variables.
 */
typedef struct NetmapThreadVars_
{
    /* receive inteface */
    NetmapDevice *ifsrc;
    /* dst interface for IPS mode */
    NetmapDevice *ifdst;

    int ring_from;
    int ring_to;
    int thread_idx;
    int flags;
    struct bpf_program bpf_prog;

    /* internal shit */
    TmSlot *slot;
    ThreadVars *tv;
    LiveDevice *livedev;

    /* copy from config */
    int copy_mode;
    ChecksumValidationMode checksum_mode;

    /* counters */
    uint64_t pkts;
    uint64_t bytes;
    uint64_t drops;
    uint16_t capture_kernel_packets;
    uint16_t capture_kernel_drops;


} NetmapThreadVars;

typedef TAILQ_HEAD(NetmapDeviceList_, NetmapDevice_) NetmapDeviceList;

static NetmapDeviceList netmap_devlist = TAILQ_HEAD_INITIALIZER(netmap_devlist);
static SCMutex netmap_devlist_lock = SCMUTEX_INITIALIZER;

/**
 * \brief Get interface flags.
 * \param fd Network susbystem file descritor.
 * \param ifname Inteface name.
 * \return Interface flags or -1 on error
 */
static int NetmapGetIfaceFlags(int fd, const char *ifname)
{
    struct ifreq ifr;

    memset(&ifr, 0, sizeof(ifr));
    strlcpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name));

    if (ioctl(fd, SIOCGIFFLAGS, &ifr) == -1) {
        SCLogError(SC_ERR_NETMAP_CREATE,
                   "Unable to get flags for iface \"%s\": %s",
                   ifname, strerror(errno));
        return -1;
    }

#ifdef OS_FREEBSD
    int flags = (ifr.ifr_flags & 0xffff) | (ifr.ifr_flagshigh << 16);
    return flags;
#else
    return ifr.ifr_flags;
#endif
}

/**
 * \brief Set interface flags.
 * \param fd Network susbystem file descritor.
 * \param ifname Inteface name.
 * \param flags Flags to set.
 * \return Zero on success.
 */
static int NetmapSetIfaceFlags(int fd, const char *ifname, int flags)
{
    struct ifreq ifr;

    memset(&ifr, 0, sizeof(ifr));
    strlcpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name));
#ifdef OS_FREEBSD
    ifr.ifr_flags = flags & 0xffff;
    ifr.ifr_flagshigh = flags >> 16;
#else
    ifr.ifr_flags = flags;
#endif

    if (ioctl(fd, SIOCSIFFLAGS, &ifr) == -1) {
        SCLogError(SC_ERR_NETMAP_CREATE,
                   "Unable to set flags for iface \"%s\": %s",
                   ifname, strerror(errno));
        return -1;
    }

    return 0;
}

/**
 * \brief Open interface in netmap mode.
 * \param ifname Interface name.
 * \param promisc Enable promiscuous mode.
 * \param dev Pointer to requested netmap device instance.
 * \param verbose Verbose error logging.
 * \return Zero on success.
 */
static int NetmapOpen(char *ifname, int promisc, NetmapDevice **pdevice, int verbose)
{
    NetmapDevice *pdev = NULL;
    struct nmreq nm_req;

    *pdevice = NULL;

    SCMutexLock(&netmap_devlist_lock);

    /* search interface in our already opened list */
    TAILQ_FOREACH(pdev, &netmap_devlist, next) {
        if (strcmp(ifname, pdev->ifname) == 0) {
            *pdevice = pdev;
            pdev->ref++;
            SCMutexUnlock(&netmap_devlist_lock);
            return 0;
        }
    }

    /* not found, create new record */
    pdev = SCMalloc(sizeof(*pdev));
    if (unlikely(pdev == NULL)) {
        SCLogError(SC_ERR_MEM_ALLOC, "Memory allocation failed");
        goto error;
    }

    memset(pdev, 0, sizeof(*pdev));
    SC_ATOMIC_INIT(pdev->threads_run);
    strlcpy(pdev->ifname, ifname, sizeof(pdev->ifname));

    /* open netmap */
    int fd = open("/dev/netmap", O_RDWR);
    if (fd == -1) {
        SCLogError(SC_ERR_NETMAP_CREATE,
                   "Couldn't open netmap device, error %s",
                   strerror(errno));
        goto error_pdev;
    }

    /* check interface is up */
    int if_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (if_fd < 0) {
        SCLogError(SC_ERR_NETMAP_CREATE,
                   "Couldn't create control socket for '%s' interface",
                   ifname);
        goto error_fd;
    }
    int if_flags = NetmapGetIfaceFlags(if_fd, ifname);
    if (if_flags == -1) {
        if (verbose) {
            SCLogError(SC_ERR_NETMAP_CREATE,
                       "Can not access to interface '%s'",
                       ifname);
        }
        close(if_fd);
        goto error_fd;
    }
    if ((if_flags & IFF_UP) == 0) {
        if (verbose) {
            SCLogError(SC_ERR_NETMAP_CREATE, "Interface '%s' is down", ifname);
        }
        close(if_fd);
        goto error_fd;
    }
    if (promisc) {
        if_flags |= IFF_PROMISC;
        NetmapSetIfaceFlags(if_fd, ifname, if_flags);
    }
    close(if_fd);

    /* query netmap info */
    memset(&nm_req, 0, sizeof(nm_req));
    strlcpy(nm_req.nr_name, ifname, sizeof(nm_req.nr_name));
    nm_req.nr_version = NETMAP_API;

    if (ioctl(fd, NIOCGINFO, &nm_req) != 0) {
        if (verbose) {
            SCLogError(SC_ERR_NETMAP_CREATE,
                       "Couldn't query netmap for %s, error %s",
                       ifname, strerror(errno));
        }
        goto error_fd;
    };
    if (nm_req.nr_rx_rings != nm_req.nr_tx_rings) {
        SCLogError(SC_ERR_NETMAP_CREATE,
                   "Interface '%s' have non-equeal Tx/Rx rings (%"PRIu16"/%"PRIu16")",
                   ifname, nm_req.nr_rx_rings, nm_req.nr_tx_rings);
        goto error_fd;
    }

    pdev->rings_cnt = nm_req.nr_rx_rings;
    pdev->memsize = nm_req.nr_memsize;

    pdev->rings = SCMalloc(sizeof(*pdev->rings) * pdev->rings_cnt);
    if (unlikely(pdev->rings == NULL)) {
        SCLogError(SC_ERR_MEM_ALLOC, "Memory allocation failed");
        goto error_fd;
    }
    memset(pdev->rings, 0, sizeof(*pdev->rings) * pdev->rings_cnt);

    /* open individual instance for each ring */
    int success_cnt = 0;
    for (int i = 0; i < pdev->rings_cnt; i++) {
        NetmapRing *pring = &pdev->rings[i];
        pring->fd = open("/dev/netmap", O_RDWR);
        if (pring->fd == -1) {
            SCLogError(SC_ERR_NETMAP_CREATE,
                       "Couldn't open netmap device: %s",
                       strerror(errno));
            break;
        }

        nm_req.nr_flags = NR_REG_ONE_NIC;
        nm_req.nr_ringid = i | NETMAP_NO_TX_POLL;
        if (ioctl(pring->fd, NIOCREGIF, &nm_req) != 0) {
            SCLogError(SC_ERR_NETMAP_CREATE,
                       "Couldn't register %s with netmap: %s",
                       ifname, strerror(errno));
            break;
        }

        if (pdev->mem == NULL) {
            pdev->mem = mmap(0, pdev->memsize, PROT_WRITE | PROT_READ,
                             MAP_SHARED, pring->fd, 0);
            if (pdev->mem == MAP_FAILED) {
                SCLogError(SC_ERR_NETMAP_CREATE,
                           "Couldn't mmap netmap device: %s",
                           strerror(errno));
                goto error_fd;
            }
            pdev->nif = NETMAP_IF(pdev->mem, nm_req.nr_offset);
        }

        pring->rx = NETMAP_RXRING(pdev->nif, i);
        pring->tx = NETMAP_TXRING(pdev->nif, i);
        SCSpinInit(&pring->tx_lock, 0);
        success_cnt++;
    }

    if (success_cnt != pdev->rings_cnt) {
        for(int i = 0; i < success_cnt; i++) {
            close(pdev->rings[i].fd);
        }
        SCFree(pdev->rings);
        goto error_mem;
    }

    close(fd);
    *pdevice = pdev;

    TAILQ_INSERT_TAIL(&netmap_devlist, pdev, next);
    SCMutexUnlock(&netmap_devlist_lock);

    return 0;

error_mem:
    munmap(pdev->mem, pdev->memsize);
error_fd:
    close(fd);
error_pdev:
    SCFree(pdev);
error:
    SCMutexUnlock(&netmap_devlist_lock);
    return -1;
}

/**
 * \brief Close or dereference netmap device instance.
 * \param pdev Netmap device instance.
 * \return Zero on success.
 */
static int NetmapClose(NetmapDevice *dev)
{
    NetmapDevice *pdev, *tmp;

    SCMutexLock(&netmap_devlist_lock);

    TAILQ_FOREACH_SAFE(pdev, &netmap_devlist, next, tmp) {
        if (pdev == dev) {
            pdev->ref--;
            if (!pdev->ref) {
                munmap(pdev->mem, pdev->memsize);
                for (int i = 0; i < pdev->rings_cnt; i++) {
                    NetmapRing *pring = &pdev->rings[i];
                    close(pring->fd);
                    SCSpinDestroy(&pring->tx_lock);
                }
                SCFree(pdev->rings);
                TAILQ_REMOVE(&netmap_devlist, pdev, next);
                SCFree(pdev);
            }
            SCMutexUnlock(&netmap_devlist_lock);
            return 0;
        }
    }

    SCMutexUnlock(&netmap_devlist_lock);
    return -1;
}

/**
 * \brief PcapDumpCounters
 * \param ntv
 */
static inline void NetmapDumpCounters(NetmapThreadVars *ntv)
{
    StatsAddUI64(ntv->tv, ntv->capture_kernel_packets, ntv->pkts);
    StatsAddUI64(ntv->tv, ntv->capture_kernel_drops, ntv->drops);
    (void) SC_ATOMIC_ADD(ntv->livedev->drop, ntv->drops);
    (void) SC_ATOMIC_ADD(ntv->livedev->pkts, ntv->pkts);
    ntv->drops = 0;
    ntv->pkts = 0;
}

/**
 * \brief Init function for ReceiveNetmap.
 * \param tv pointer to ThreadVars
 * \param initdata pointer to the interface passed from the user
 * \param data pointer gets populated with NetmapThreadVars
 */
static TmEcode ReceiveNetmapThreadInit(ThreadVars *tv, void *initdata, void **data)
{
    SCEnter();
    NetmapIfaceConfig *aconf = initdata;

    if (initdata == NULL) {
        SCLogError(SC_ERR_INVALID_ARGUMENT, "initdata == NULL");
        SCReturnInt(TM_ECODE_FAILED);
    }

    NetmapThreadVars *ntv = SCMalloc(sizeof(*ntv));
    if (unlikely(ntv == NULL)) {
        SCLogError(SC_ERR_MEM_ALLOC, "Memory allocation failed");
        goto error;
    }
    memset(ntv, 0, sizeof(*ntv));

    ntv->tv = tv;
    ntv->checksum_mode = aconf->checksum_mode;
    ntv->copy_mode = aconf->copy_mode;

    ntv->livedev = LiveGetDevice(aconf->iface);
    if (ntv->livedev == NULL) {
        SCLogError(SC_ERR_INVALID_VALUE, "Unable to find Live device");
        goto error_ntv;
    }

    if (NetmapOpen(aconf->iface, aconf->promisc, &ntv->ifsrc, 1) != 0) {
        goto error_ntv;
    }

    if (aconf->threads > ntv->ifsrc->rings_cnt) {
        SCLogError(SC_ERR_INVALID_VALUE,
                   "Thread count can't be greater than ring count. "
                   "Configured %d threads for interfaces '%s' with %u rings.",
                   aconf->threads, aconf->iface, ntv->ifsrc->rings_cnt);
        goto error_src;
    }

    do {
        ntv->thread_idx = SC_ATOMIC_GET(ntv->ifsrc->threads_run);
    } while (SC_ATOMIC_CAS(&ntv->ifsrc->threads_run, ntv->thread_idx, ntv->thread_idx + 1) == 0);

    /* calculate rings borders */
    int tmp = ntv->ifsrc->rings_cnt / aconf->threads;
    ntv->ring_from = ntv->thread_idx * tmp;
    ntv->ring_to = ntv->ring_from + tmp - 1;
    if (ntv->ring_to >= ntv->ifsrc->rings_cnt)
        ntv->ring_to = ntv->ifsrc->rings_cnt - 1;

    if (aconf->copy_mode != NETMAP_COPY_MODE_NONE) {
        if (NetmapOpen(aconf->out_iface, 0, &ntv->ifdst, 1) != 0) {
            goto error_src;
        }
    }

    /* basic counters */
    ntv->capture_kernel_packets = StatsRegisterCounter("capture.kernel_packets",
            ntv->tv);
    ntv->capture_kernel_drops = StatsRegisterCounter("capture.kernel_drops",
            ntv->tv);

    char const *active_runmode = RunmodeGetActive();
    if (active_runmode && !strcmp("workers", active_runmode)) {
        ntv->flags |= NETMAP_FLAG_ZERO_COPY;
        SCLogInfo("Enabling zero copy mode");
    }

    if (aconf->bpf_filter) {
        SCLogInfo("Using BPF '%s' on iface '%s'",
                  aconf->bpf_filter, ntv->ifsrc->ifname);
        if (pcap_compile_nopcap(default_packet_size,  /* snaplen_arg */
                    LINKTYPE_ETHERNET,    /* linktype_arg */
                    &ntv->bpf_prog,       /* program */
                    aconf->bpf_filter,    /* const char *buf */
                    1,                    /* optimize */
                    PCAP_NETMASK_UNKNOWN  /* mask */
                    ) == -1) {
            SCLogError(SC_ERR_NETMAP_CREATE, "Filter compilation failed.");
            goto error_src;
        }
    }

    if (GetIfaceOffloading(aconf->iface) == 1) {
        SCLogWarning(SC_ERR_NETMAP_CREATE,
                     "Using mmap mode with GRO or LRO activated can lead to capture problems");
    }

    *data = (void *)ntv;
    aconf->DerefFunc(aconf);
    SCReturnInt(TM_ECODE_OK);

error_src:
    NetmapClose(ntv->ifsrc);
error_ntv:
    SCFree(ntv);
error:
    aconf->DerefFunc(aconf);
    SCReturnInt(TM_ECODE_FAILED);
}

/**
 * \brief Output packet to destination interface or drop.
 * \param ntv Thread local variables.
 * \param p Source packet.
 */
static TmEcode NetmapWritePacket(NetmapThreadVars *ntv, Packet *p)
{
    if (ntv->copy_mode == NETMAP_COPY_MODE_IPS) {
        if (PACKET_TEST_ACTION(p, ACTION_DROP)) {
            return TM_ECODE_OK;
        }
    }

    /* map src ring_id to dst ring_id */
    int dst_ring_id = p->netmap_v.ring_id % ntv->ifdst->rings_cnt;
    NetmapRing *txring = &ntv->ifdst->rings[dst_ring_id];
    NetmapRing *rxring = &ntv->ifsrc->rings[p->netmap_v.ring_id];

    SCSpinLock(&txring->tx_lock);

    if (!nm_ring_space(txring->tx)) {
        ntv->drops++;
        SCSpinUnlock(&txring->tx_lock);
        return TM_ECODE_FAILED;
    }

    struct netmap_slot *rs = &rxring->rx->slot[p->netmap_v.slot_id];
    struct netmap_slot *ts = &txring->tx->slot[txring->tx->cur];

    /* swap slot buffers */
    uint32_t tmp_idx;
    tmp_idx = ts->buf_idx;
    ts->buf_idx = rs->buf_idx;
    rs->buf_idx = tmp_idx;

    ts->len = rs->len;

    ts->flags |= NS_BUF_CHANGED;
    rs->flags |= NS_BUF_CHANGED;

    txring->tx->head = txring->tx->cur = nm_ring_next(txring->tx, txring->tx->cur);

    SCSpinUnlock(&txring->tx_lock);

    return TM_ECODE_OK;
}

/**
 * \brief Packet release routine.
 * \param p Packet.
 */
static void NetmapReleasePacket(Packet *p)
{
    NetmapThreadVars *ntv = (NetmapThreadVars *)p->netmap_v.ntv;

    /* Need to be in copy mode and need to detect early release
       where Ethernet header could not be set (and pseudo packet) */
    if ((ntv->copy_mode != NETMAP_COPY_MODE_NONE) && !PKT_IS_PSEUDOPKT(p)) {
        NetmapWritePacket(ntv, p);
    }

    PacketFreeOrRelease(p);
}

/**
 * \brief Read packets from ring and pass them further.
 * \param ntv Thread local variables.
 * \param ring_id Ring id to read.
 */
static int NetmapRingRead(NetmapThreadVars *ntv, int ring_id)
{
    SCEnter();

    struct netmap_ring *ring = ntv->ifsrc->rings[ring_id].rx;
    uint32_t avail = nm_ring_space(ring);
    uint32_t cur = ring->cur;

    while (likely(avail-- > 0)) {
        struct netmap_slot *slot = &ring->slot[cur];
        unsigned char *slot_data = (unsigned char *)NETMAP_BUF(ring, slot->buf_idx);

        if (ntv->bpf_prog.bf_len) {
            struct pcap_pkthdr pkthdr = { {0, 0}, slot->len, slot->len };
            if (pcap_offline_filter(&ntv->bpf_prog, &pkthdr, slot_data) == 0) {
                /* rejected by bpf */
                cur = nm_ring_next(ring, cur);
                continue;
            }
        }

        Packet *p = PacketGetFromQueueOrAlloc();
        if (unlikely(p == NULL)) {
            SCReturnInt(NETMAP_FAILURE);
        }

        PKT_SET_SRC(p, PKT_SRC_WIRE);
        p->livedev = ntv->livedev;
        p->datalink = LINKTYPE_ETHERNET;
        p->ts = ring->ts;
        ntv->pkts++;
        ntv->bytes += slot->len;

        /* checksum validation */
        if (ntv->checksum_mode == CHECKSUM_VALIDATION_DISABLE) {
            p->flags |= PKT_IGNORE_CHECKSUM;
        } else if (ntv->checksum_mode == CHECKSUM_VALIDATION_AUTO) {
            if (ntv->livedev->ignore_checksum) {
                p->flags |= PKT_IGNORE_CHECKSUM;
            } else if (ChecksumAutoModeCheck(ntv->pkts,
                        SC_ATOMIC_GET(ntv->livedev->pkts),
                        SC_ATOMIC_GET(ntv->livedev->invalid_checksums))) {
                ntv->livedev->ignore_checksum = 1;
                p->flags |= PKT_IGNORE_CHECKSUM;
            }
        }

        if (ntv->flags & NETMAP_FLAG_ZERO_COPY) {
            if (PacketSetData(p, slot_data, slot->len) == -1) {
                TmqhOutputPacketpool(ntv->tv, p);
                SCReturnInt(NETMAP_FAILURE);
            } else {
                p->ReleasePacket = NetmapReleasePacket;
                p->netmap_v.ring_id = ring_id;
                p->netmap_v.slot_id = cur;
                p->netmap_v.ntv = ntv;
            }
        } else {
            if (PacketCopyData(p, slot_data, slot->len) == -1) {
                TmqhOutputPacketpool(ntv->tv, p);
                SCReturnInt(NETMAP_FAILURE);
            }
        }

        SCLogDebug("pktlen: %" PRIu32 " (pkt %p, pkt data %p)",
                   GET_PKT_LEN(p), p, GET_PKT_DATA(p));

        if (TmThreadsSlotProcessPkt(ntv->tv, ntv->slot, p) != TM_ECODE_OK) {
            TmqhOutputPacketpool(ntv->tv, p);
            SCReturnInt(NETMAP_FAILURE);
        }

        cur = nm_ring_next(ring, cur);
    }
    ring->head = ring->cur = cur;

    SCReturnInt(NETMAP_OK);
}

/**
 *  \brief Main netmap reading loop function
 */
static TmEcode ReceiveNetmapLoop(ThreadVars *tv, void *data, void *slot)
{
    SCEnter();

    TmSlot *s = (TmSlot *)slot;
    NetmapThreadVars *ntv = (NetmapThreadVars *)data;
    struct pollfd *fds;
    int rings_count = ntv->ring_to - ntv->ring_from + 1;

    ntv->slot = s->slot_next;

    fds = SCMalloc(sizeof(*fds) * rings_count);
    if (unlikely(fds == NULL)) {
        SCLogError(SC_ERR_MEM_ALLOC, "Memory allocation failed");
        SCReturnInt(TM_ECODE_FAILED);
    }

    for (int i = 0; i < rings_count; i++) {
        fds[i].fd = ntv->ifsrc->rings[ntv->ring_from + i].fd;
        fds[i].events = POLLIN;
    }

    for(;;) {
        if (suricata_ctl_flags != 0) {
            break;
        }

        /* make sure we have at least one packet in the packet pool,
         * to prevent us from alloc'ing packets at line rate */
        PacketPoolWait();

        int r = poll(fds, rings_count, POLL_TIMEOUT);

        if (r < 0) {
            /* error */
            if(errno != EINTR)
                SCLogError(SC_ERR_NETMAP_READ,
                           "Error polling netmap from iface '%s': (%d" PRIu32 ") %s",
                           ntv->ifsrc->ifname, errno, strerror(errno));
            continue;
        } else if (r == 0) {
            /* no events, timeout */
            SCLogDebug("(%s:%d-%d) Poll timeout", ntv->ifsrc->ifname,
                       ntv->ring_from, ntv->ring_to);
            continue;
        }

        for (int i = 0; i < rings_count; i++) {
            if (fds[i].revents & POLL_EVENTS) {
                if (fds[i].revents & POLLERR) {
                    SCLogError(SC_ERR_NETMAP_READ,
                               "Error reading data from iface '%s': (%d" PRIu32 ") %s",
                               ntv->ifsrc->ifname, errno, strerror(errno));
                } else if (fds[i].revents & POLLNVAL) {
                    SCLogError(SC_ERR_NETMAP_READ,
                               "Invalid polling request");
                }
                continue;
            }

            if (likely(fds[i].revents & POLLIN)) {
                int src_ring_id = ntv->ring_from + i;
                NetmapRingRead(ntv, src_ring_id);

                if (ntv->copy_mode != NETMAP_COPY_MODE_NONE) {
                    /* sync dst tx rings */
                    int dst_ring_id = src_ring_id % ntv->ifdst->rings_cnt;
                    NetmapRing *dst_ring = &ntv->ifdst->rings[dst_ring_id];
                    if (SCSpinTrylock(&dst_ring->tx_lock) == 0) {
                        ioctl(dst_ring->fd, NIOCTXSYNC, 0);
                        SCSpinUnlock(&dst_ring->tx_lock);
                    }
                }
            }
        }

        NetmapDumpCounters(ntv);
        StatsSyncCountersIfSignalled(tv);
    }

    SCFree(fds);
    StatsSyncCountersIfSignalled(tv);
    SCReturnInt(TM_ECODE_OK);
}

/**
 * \brief This function prints stats to the screen at exit.
 * \param tv pointer to ThreadVars
 * \param data pointer that gets cast into NetmapThreadVars for ntv
 */
static void ReceiveNetmapThreadExitStats(ThreadVars *tv, void *data)
{
    SCEnter();
    NetmapThreadVars *ntv = (NetmapThreadVars *)data;

    NetmapDumpCounters(ntv);
    SCLogInfo("(%s) Kernel: Packets %" PRIu64 ", dropped %" PRIu64 ", bytes %" PRIu64 "",
              tv->name,
              StatsGetLocalCounterValue(tv, ntv->capture_kernel_packets),
              StatsGetLocalCounterValue(tv, ntv->capture_kernel_drops),
              ntv->bytes);
}

/**
 * \brief
 * \param tv
 * \param data Pointer to NetmapThreadVars.
 */
static TmEcode ReceiveNetmapThreadDeinit(ThreadVars *tv, void *data)
{
    SCEnter();

    NetmapThreadVars *ntv = (NetmapThreadVars *)data;

    if (ntv->ifsrc) {
        NetmapClose(ntv->ifsrc);
        ntv->ifsrc = NULL;
    }
    if (ntv->ifdst) {
        NetmapClose(ntv->ifdst);
        ntv->ifdst = NULL;
    }
    if (ntv->bpf_prog.bf_insns) {
        pcap_freecode(&ntv->bpf_prog);
    }

    SCReturnInt(TM_ECODE_OK);
}

/**
 * \brief Prepare netmap decode thread.
 * \param tv Thread local avariables.
 * \param initdata Thread config.
 * \param data Pointer to DecodeThreadVars placed here.
 */
static TmEcode DecodeNetmapThreadInit(ThreadVars *tv, void *initdata, void **data)
{
    SCEnter();
    DecodeThreadVars *dtv = NULL;

    dtv = DecodeThreadVarsAlloc(tv);

    if (dtv == NULL)
        SCReturnInt(TM_ECODE_FAILED);

    DecodeRegisterPerfCounters(dtv, tv);

    *data = (void *)dtv;

#ifdef __SC_CUDA_SUPPORT__
    if (CudaThreadVarsInit(&dtv->cuda_vars) < 0)
        SCReturnInt(TM_ECODE_FAILED);
#endif

    SCReturnInt(TM_ECODE_OK);
}

/**
 * \brief This function passes off to link type decoders.
 *
 * DecodeNetmap reads packets from the PacketQueue and passes
 * them off to the proper link type decoder.
 *
 * \param t pointer to ThreadVars
 * \param p pointer to the current packet
 * \param data pointer that gets cast into NetmapThreadVars for ntv
 * \param pq pointer to the current PacketQueue
 * \param postpq
 */
static TmEcode DecodeNetmap(ThreadVars *tv, Packet *p, void *data, PacketQueue *pq, PacketQueue *postpq)
{
    SCEnter();

    DecodeThreadVars *dtv = (DecodeThreadVars *)data;

    /* XXX HACK: flow timeout can call us for injected pseudo packets
     *           see bug: https://redmine.openinfosecfoundation.org/issues/1107 */
    if (p->flags & PKT_PSEUDO_STREAM_END)
        SCReturnInt(TM_ECODE_OK);

    /* update counters */
    StatsIncr(tv, dtv->counter_pkts);
    StatsAddUI64(tv, dtv->counter_bytes, GET_PKT_LEN(p));
    StatsAddUI64(tv, dtv->counter_avg_pkt_size, GET_PKT_LEN(p));
    StatsSetUI64(tv, dtv->counter_max_pkt_size, GET_PKT_LEN(p));

    DecodeEthernet(tv, dtv, p, GET_PKT_DATA(p), GET_PKT_LEN(p), pq);

    PacketDecodeFinalize(tv, dtv, p);

    SCReturnInt(TM_ECODE_OK);
}

/**
 * \brief
 * \param tv
 * \param data Pointer to DecodeThreadVars.
 */
static TmEcode DecodeNetmapThreadDeinit(ThreadVars *tv, void *data)
{
    SCEnter();

    if (data != NULL)
        DecodeThreadVarsFree(tv, data);

    SCReturnInt(TM_ECODE_OK);
}

/**
 * \brief Registration Function for RecieveNetmap.
 */
void TmModuleReceiveNetmapRegister(void)
{
    tmm_modules[TMM_RECEIVENETMAP].name = "ReceiveNetmap";
    tmm_modules[TMM_RECEIVENETMAP].ThreadInit = ReceiveNetmapThreadInit;
    tmm_modules[TMM_RECEIVENETMAP].Func = NULL;
    tmm_modules[TMM_RECEIVENETMAP].PktAcqLoop = ReceiveNetmapLoop;
    tmm_modules[TMM_RECEIVENETMAP].ThreadExitPrintStats = ReceiveNetmapThreadExitStats;
    tmm_modules[TMM_RECEIVENETMAP].ThreadDeinit = ReceiveNetmapThreadDeinit;
    tmm_modules[TMM_RECEIVENETMAP].RegisterTests = NULL;
    tmm_modules[TMM_RECEIVENETMAP].cap_flags = SC_CAP_NET_RAW;
    tmm_modules[TMM_RECEIVENETMAP].flags = TM_FLAG_RECEIVE_TM;
}

/**
 * \brief Registration Function for DecodeNetmap.
 */
void TmModuleDecodeNetmapRegister(void)
{
    tmm_modules[TMM_DECODENETMAP].name = "DecodeNetmap";
    tmm_modules[TMM_DECODENETMAP].ThreadInit = DecodeNetmapThreadInit;
    tmm_modules[TMM_DECODENETMAP].Func = DecodeNetmap;
    tmm_modules[TMM_DECODENETMAP].ThreadExitPrintStats = NULL;
    tmm_modules[TMM_DECODENETMAP].ThreadDeinit = DecodeNetmapThreadDeinit;
    tmm_modules[TMM_DECODENETMAP].RegisterTests = NULL;
    tmm_modules[TMM_DECODENETMAP].cap_flags = 0;
    tmm_modules[TMM_DECODENETMAP].flags = TM_FLAG_DECODE_TM;
}

#endif /* HAVE_NETMAP */
/* eof */
/**
* @}
*/