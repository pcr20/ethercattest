#include "nic.h"
#include "stats.h"

/* ── NIC MAC address ────────────────────────────────────────────────────── */
int get_mac(int sock, const char *iface, uint8_t *mac) {
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);
    if (ioctl(sock, SIOCGIFHWADDR, &ifr) < 0) return -1;
    memcpy(mac, ifr.ifr_hwaddr.sa_data, 6);
    return 0;
}

/* ── Interface index ────────────────────────────────────────────────────── */
int get_ifindex(int sock, const char *iface) {
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);
    if (ioctl(sock, SIOCGIFINDEX, &ifr) < 0) return -1;
    return ifr.ifr_ifindex;
}

/* ── ethtool counter reader ─────────────────────────────────────────────────
 * Generic helper: sum all ethtool -S counters whose name contains ANY of the
 * given substrings (case-insensitive on the needles as written). Used for both
 * CRC/FCS error counting and TX-packet (on-wire) counting across chipsets.
 *
 * exclude, if non-NULL, is a substring that disqualifies a match (e.g. exclude
 * "err" when summing tx_packets so we don't pick up tx_*_errors). */
static uint64_t ethtool_sum_counters(const char *iface,
                                     const char *const *needles, int n_needles,
                                     const char *exclude) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return 0;

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);

    struct ethtool_drvinfo drvinfo;
    drvinfo.cmd = ETHTOOL_GDRVINFO;
    ifr.ifr_data = (void *)&drvinfo;
    if (ioctl(fd, SIOCETHTOOL, &ifr) < 0) { close(fd); return 0; }

    uint32_t n_stats = drvinfo.n_stats;
    if (n_stats == 0) { close(fd); return 0; }

    size_t sset_size = sizeof(struct ethtool_gstrings) + n_stats * ETH_GSTRING_LEN;
    struct ethtool_gstrings *gstrings = calloc(1, sset_size);
    if (!gstrings) { close(fd); return 0; }
    gstrings->cmd = ETHTOOL_GSTRINGS;
    gstrings->string_set = ETH_SS_STATS;
    gstrings->len = n_stats;
    ifr.ifr_data = (void *)gstrings;
    if (ioctl(fd, SIOCETHTOOL, &ifr) < 0) { free(gstrings); close(fd); return 0; }

    size_t stats_size = sizeof(struct ethtool_stats) + n_stats * sizeof(uint64_t);
    struct ethtool_stats *stats = calloc(1, stats_size);
    if (!stats) { free(gstrings); close(fd); return 0; }
    stats->cmd = ETHTOOL_GSTATS;
    stats->n_stats = n_stats;
    ifr.ifr_data = (void *)stats;
    if (ioctl(fd, SIOCETHTOOL, &ifr) < 0) { free(gstrings); free(stats); close(fd); return 0; }

    uint64_t total = 0;
    for (uint32_t i = 0; i < n_stats; i++) {
        char name[ETH_GSTRING_LEN + 1];
        memcpy(name, gstrings->data + i * ETH_GSTRING_LEN, ETH_GSTRING_LEN);
        name[ETH_GSTRING_LEN] = '\0';
        if (exclude && strstr(name, exclude)) continue;
        for (int k = 0; k < n_needles; k++) {
            if (strstr(name, needles[k])) { total += stats->data[i]; break; }
        }
    }

    free(gstrings);
    free(stats);
    close(fd);
    return total;
}

/* On-wire TX packet counter. Sums the driver's transmitted-packet counters,
 * excluding any *error* counters. Names vary: r8169/RTL8125 exposes
 * "tx_packets"; some drivers use "tx_unicast"+"tx_multicast"+"tx_broadcast".
 * We prefer an exact "tx_packets" if present, else fall back to the sum of the
 * per-cast counters. Returns 0 if none found (caller notes unavailability). */
uint64_t read_nic_tx_packets(const char *iface) {
    /* First try the single authoritative counter. */
    static const char *const exact[] = { "tx_packets" };
    uint64_t v = ethtool_sum_counters(iface, exact, 1, "err");
    if (v) return v;
    /* Fall back to per-cast counters. */
    static const char *const cast[] = { "tx_unicast", "tx_multicast", "tx_broadcast" };
    return ethtool_sum_counters(iface, cast, 3, "err");
}

/* On-wire TX ERROR counter (TxER: Tx errors incl. carrier-lost, abort,
 * underrun, out-of-window collision). These frames reached the r8169 MAC but
 * were NOT successfully transmitted — they are at the measurement boundary
 * (r8169) but are link-down collateral, NOT physical-layer bit-error loss, so
 * they are reported separately and excluded from BER. */
uint64_t read_nic_tx_errors(const char *iface) {
    static const char *const needles[] = { "tx_errors", "tx_err" };
    return ethtool_sum_counters(iface, needles, 2, NULL);
}

/* Read a uint64 from /sys/class/net/<iface>/<name>. Returns 0 and sets *ok=0
 * on failure (file missing / unreadable). */
uint64_t read_sysfs_u64(const char *iface, const char *name, int *ok) {
    char path[256];
    snprintf(path, sizeof(path), "/sys/class/net/%s/%s", iface, name);
    FILE *f = fopen(path, "r");
    if (!f) { if (ok) *ok = 0; return 0; }
    uint64_t v = 0;
    int n = fscanf(f, "%lu", &v);
    fclose(f);
    if (ok) *ok = (n == 1);
    return (n == 1) ? v : 0;
}

/* Query whether an ethtool feature (by name, e.g. "rx-fcs", "rx-all") is
 * active. Uses ETHTOOL_GSSET_INFO + ETHTOOL_GSTRINGS(ETH_SS_FEATURES) to find
 * the bit index, then ETHTOOL_GFEATURES to read it. Returns 1 if on, 0 if off
 * or unknown, and sets *known=0 if the feature name wasn't found. */
int ethtool_feature_on(const char *iface, const char *feat, int *known) {
    if (known) *known = 0;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return 0;
    struct ifreq ifr; memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);

    /* Number of feature strings. */
    struct { struct ethtool_sset_info hdr; uint32_t buf; } si;
    memset(&si, 0, sizeof(si));
    si.hdr.cmd = ETHTOOL_GSSET_INFO;
    si.hdr.sset_mask = 1ULL << ETH_SS_FEATURES;
    ifr.ifr_data = (void *)&si;
    if (ioctl(fd, SIOCETHTOOL, &ifr) < 0 || si.hdr.sset_mask == 0) { close(fd); return 0; }
    uint32_t nfeat = si.buf;
    if (!nfeat) { close(fd); return 0; }

    size_t gs_sz = sizeof(struct ethtool_gstrings) + (size_t)nfeat * ETH_GSTRING_LEN;
    struct ethtool_gstrings *gs = calloc(1, gs_sz);
    if (!gs) { close(fd); return 0; }
    gs->cmd = ETHTOOL_GSTRINGS; gs->string_set = ETH_SS_FEATURES; gs->len = nfeat;
    ifr.ifr_data = (void *)gs;
    if (ioctl(fd, SIOCETHTOOL, &ifr) < 0) { free(gs); close(fd); return 0; }

    int idx = -1;
    for (uint32_t i = 0; i < nfeat; i++) {
        char name[ETH_GSTRING_LEN + 1];
        memcpy(name, gs->data + i * ETH_GSTRING_LEN, ETH_GSTRING_LEN);
        name[ETH_GSTRING_LEN] = '\0';
        if (strcmp(name, feat) == 0) { idx = (int)i; break; }
    }
    free(gs);
    if (idx < 0) { close(fd); return 0; }   /* feature name not present */

    uint32_t nblocks = (nfeat + 31) / 32;
    size_t gf_sz = sizeof(struct ethtool_gfeatures)
                 + (size_t)nblocks * sizeof(struct ethtool_get_features_block);
    struct ethtool_gfeatures *gf = calloc(1, gf_sz);
    if (!gf) { close(fd); return 0; }
    gf->cmd = ETHTOOL_GFEATURES; gf->size = nblocks;
    ifr.ifr_data = (void *)gf;
    int on = 0;
    if (ioctl(fd, SIOCETHTOOL, &ifr) == 0) {
        uint32_t blk = idx / 32, bit = idx % 32;
        on = (gf->features[blk].active >> bit) & 1;
        if (known) *known = 1;
    }
    free(gf);
    close(fd);
    return on;
}


/* Poll kernel RX-drop statistics (queue overflow). Accumulates into
 * g_stats.kernel_drops. Reading PACKET_STATISTICS resets the kernel's
 * internal counters, so each read returns the delta since the last. */
void poll_kernel_drops(int sock) {
    struct tpacket_stats st;
    socklen_t len = sizeof(st);
    if (getsockopt(sock, SOL_PACKET, PACKET_STATISTICS, &st, &len) == 0) {
        if (st.tp_drops)
            atomic_fetch_add_explicit(&g_stats.kernel_drops, st.tp_drops,
                                      memory_order_relaxed);
    }
}
/* ── Software TX timestamping ────────────────────────────────────────────────
 * This NIC (RTL8125) has no PTP hardware clock, so only SOFTWARE TX
 * timestamping is available. The timestamp is taken in the kernel driver's
 * xmit path — strictly UPSTREAM of the wire — so this counter measures
 * "frames the driver pushed toward hardware", not true on-wire time. The
 * ethtool tx_packets counter remains the authoritative on-wire figure.
 *
 * Enable and return 1 on success, 0 if unsupported. */
int enable_tx_timestamping(int sock) {
    int flags = SOF_TIMESTAMPING_TX_SOFTWARE   /* sw timestamp on TX */
              | SOF_TIMESTAMPING_SOFTWARE       /* report sw timestamps */
              | SOF_TIMESTAMPING_OPT_ID         /* per-frame id */
              | SOF_TIMESTAMPING_OPT_TSONLY;    /* don't copy frame back */
    if (setsockopt(sock, SOL_SOCKET, SO_TIMESTAMPING, &flags, sizeof(flags)) < 0)
        return 0;
    return 1;
}

