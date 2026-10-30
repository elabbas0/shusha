/* lanping: LAN reachability test.  Broadcasts a hello on UDP 7000 every
 * second for N seconds, answers every hello it hears with a unicast
 * reply, and lists each machine that answered.  Proves broadcast, ARP
 * resolution and unicast between Shusha machines. */
#include "lib.h"
#include "net.h"

#define PORT 7000
#define MAX_PEERS 16

int main(int argc, char **argv) {
    uint8_t me[4], mask[4], bcast[4], peers[MAX_PEERS][4];
    char ip[16], msg[64];
    int npeers = 0, seconds = argc > 1 ? (int)(argv[1][0] - '0') * 10 + (argv[1][1] ? argv[1][1] - '0' : 0) : 8;
    uint64_t end, next = 0;

    if (argc > 1 && !argv[1][1]) seconds = argv[1][0] - '0';
    if (net_local(me, mask) != 0 || net_bind(PORT) != 0) {
        print("lanping: network unavailable\n");
        return 1;
    }
    net_broadcast_addr(bcast);
    print("lanping: this machine is ");
    print(net_ip_text(ip, me));
    print("\n");

    end = sh_ticks() + (uint64_t)seconds * 100;
    while (sh_ticks() < end) {
        net_addr_t from;
        long n;
        if (sh_ticks() >= next) {
            (void)net_send(PORT, bcast, PORT, "HELLO", 5);
            next = sh_ticks() + 100;
        }
        while ((n = net_recv(msg, sizeof(msg) - 1, &from, 0)) > 0) {
            int known = 0;
            msg[n] = '\0';
            if (str_eq(msg, "HELLO")) (void)net_send(PORT, from.ip, from.port, "REPLY", 5);
            if (!str_eq(msg, "REPLY")) continue;
            for (int i = 0; i < npeers; i++) {
                if (mem_eq_ct(peers[i], from.ip, 4)) known = 1;
            }
            if (!known && npeers < MAX_PEERS) {
                mem_copy(peers[npeers++], from.ip, 4);
                print("lanping: reply from ");
                print(net_ip_text(ip, from.ip));
                print("\n");
            }
        }
        sh_sleep(2);
    }
    print("lanping: ");
    print_u64((uint64_t)npeers);
    print(" peer(s) answered\n");
    return 0;
}
