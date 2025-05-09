#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "net.h"

#define UPORT_BUCKET_SZ 101
#define UPORT_HASH(port) ((port) % (UPORT_BUCKET_SZ))
#define MAX_PENDING_PACKETS 16

struct upacket {
  struct upacket* next;
  uint64 buf;
  int len;
  int src_ip;
  uint16 src_port;
};

struct uport {
  struct uport* next;
  uint16 port;

  // below protected by the spinlock
  struct spinlock lk;
  int size;
  struct upacket* head;
  struct upacket* tail;
};

// table to manage udp
struct utable {
  struct spinlock lk;
  struct uport* ports[UPORT_BUCKET_SZ];
};

// xv6's ethernet and IP addresses
static uint8 local_mac[ETHADDR_LEN] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
static uint32 local_ip = MAKE_IP_ADDR(10, 0, 2, 15);

// qemu host's ethernet address.
static uint8 host_mac[ETHADDR_LEN] = { 0x52, 0x55, 0x0a, 0x00, 0x02, 0x02 };

static struct spinlock netlock;
static struct utable utab;

void
netinit(void)
{
  initlock(&netlock, "netlock");
  initlock(&utab.lk, "utablelock");
  for (int i = 0; i < UPORT_BUCKET_SZ; ++i) {
    utab.ports[i] = 0;
  }
}

// should hold utab.lk when called
struct uport* find_udp_port(int port)
{
  int hash = UPORT_HASH(port);
  for (struct uport* handle = utab.ports[hash]; handle != 0; handle = handle->next) {
    if (handle->port == port) {
      return handle;
    }
  }
  return 0;
}

//
// bind(int port)
// prepare to receive UDP packets address to the port,
// i.e. allocate any queues &c needed.
//
uint64
sys_bind(void)
{
  int port;

  argint(0, &port);

  acquire(&utab.lk);
  if (find_udp_port(port)) {
    printf("port %d is already being used.\n", port);
    release(&utab.lk);
    return -1;
  }

  int hash = UPORT_HASH(port);
  struct uport* up = utab.ports[hash];

  struct uport* new_port = kalloc();
  if (!new_port) {
    release(&utab.lk);
    return -1;
  }

  memset((void*)new_port, 0, sizeof(struct uport));
  new_port->next = up;
  utab.ports[hash] = new_port;
  new_port->port = port;
  initlock(&new_port->lk, "uportlock");
  new_port->size = 0;
  new_port->head = 0;
  new_port->tail = 0;
  release(&utab.lk);
  return 0;
}

//
// unbind(int port)
// release any resources previously created by bind(port);
// from now on UDP packets addressed to port should be dropped.
//
uint64
sys_unbind(void)
{
  int port;

  argint(0, &port);

  acquire(&utab.lk);
  int hash = UPORT_HASH(port);
  struct uport* prev = 0;
  struct uport* up = 0;
  for (struct uport* handle = utab.ports[hash]; handle != 0; handle = handle->next) {
    if (handle->port == port) {
      up = handle;
      break;
    }
    prev = handle;
  }

  if (!up) {
    printf("port %d is not bound.\n", port);
    return -1;
  }
  for (struct upacket* p = up->head; p != 0;) {
    struct upacket* next = p->next;
    kfree((void*)p->buf);
    kfree(p);
    p = next;
  }

  if (!prev) {
    utab.ports[hash] = up->next;
  } else {
    prev->next = up->next;
  }
  release(&utab.lk);
  kfree(up);

  return 0;
}

//
// recv(int dport, int *src, short *sport, char *buf, int maxlen)
// if there's a received UDP packet already queued that was
// addressed to dport, then return it.
// otherwise wait for such a packet.
//
// sets *src to the IP source address.
// sets *sport to the UDP source port.
// copies up to maxlen bytes of UDP payload to buf.
// returns the number of bytes copied,
// and -1 if there was an error.
//
// dport, *src, and *sport are host byte order.
// bind(dport) must previously have been called.
//
uint64
sys_recv(void)
{
  int dport;      // destination host port number
  uint64 src;     // source host IP address
  uint64 sport;   // source host port number
  uint64 bufaddr; // payload buffer address
  int maxlen;     // payload length

  argint(0, &dport);
  argaddr(1, &src);
  argaddr(2, &sport);
  argaddr(3, &bufaddr);
  argint(4, &maxlen);

  struct proc* p = myproc();

  acquire(&utab.lk);
  struct uport* up = find_udp_port(dport);

  if (!up) {
    release(&utab.lk);
    return -1;
  }

  acquire(&up->lk);
  release(&utab.lk);

  // wait until packet is available
  while (!up->size) {
    if (killed(p)) {
      release(&up->lk);
      return -1;
    }
    sleep(up, &up->lk);
  }

  struct upacket* packet = up->head;

  int len_moved = maxlen;
  if (packet->len < len_moved) {
    len_moved = packet->len;
  }

  if (copyout(p->pagetable, src, (char*)&packet->src_ip, sizeof(packet->src_ip)) < 0
      || copyout(p->pagetable, sport, (char*)&packet->src_port, sizeof(packet->src_port)) < 0
      || copyout(p->pagetable, bufaddr, (char*)packet->buf, len_moved) < 0) {
    release(&up->lk);
    kfree((void*)packet->buf);
    kfree(packet);
    return -1;
  }

  // cleanup the buffered packet
  --(up->size);
  if (!up->size) {
    up->head = 0;
    up->tail = 0;
  } else {
    up->head = packet->next;
  }

  release(&up->lk);
  kfree((void*)packet->buf);
  kfree(packet);

  return len_moved;
}

// This code is lifted from FreeBSD's ping.c, and is copyright by the Regents
// of the University of California.
static unsigned short
in_cksum(const unsigned char *addr, int len)
{
  int nleft = len;
  const unsigned short *w = (const unsigned short *)addr;
  unsigned int sum = 0;
  unsigned short answer = 0;

  /*
   * Our algorithm is simple, using a 32 bit accumulator (sum), we add
   * sequential 16 bit words to it, and at the end, fold back all the
   * carry bits from the top 16 bits into the lower 16 bits.
   */
  while (nleft > 1)  {
    sum += *w++;
    nleft -= 2;
  }

  /* mop up an odd byte, if necessary */
  if (nleft == 1) {
    *(unsigned char *)(&answer) = *(const unsigned char *)w;
    sum += answer;
  }

  /* add back carry outs from top 16 bits to low 16 bits */
  sum = (sum & 0xffff) + (sum >> 16);
  sum += (sum >> 16);
  /* guaranteed now that the lower 16 bits of sum are correct */

  answer = ~sum; /* truncate to 16 bits */
  return answer;
}

//
// send(int sport, int dst, int dport, char *buf, int len)
// 0 on success, -1 on failure
//
uint64
sys_send(void)
{
  struct proc *p = myproc();
  int sport;      // source host port number
  int dst;        // destination host IP address
  int dport;      // destination host port number
  uint64 bufaddr; // payload buffer address
  int len;        // payload length

  argint(0, &sport);
  argint(1, &dst);
  argint(2, &dport);
  argaddr(3, &bufaddr);
  argint(4, &len);

  int total = len + sizeof(struct eth) + sizeof(struct ip) + sizeof(struct udp);
  if(total > PGSIZE)
    return -1;

  char *buf = kalloc();
  if(buf == 0){
    printf("sys_send: kalloc failed\n");
    return -1;
  }
  memset(buf, 0, PGSIZE);

  struct eth *eth = (struct eth *) buf;
  memmove(eth->dhost, host_mac, ETHADDR_LEN);
  memmove(eth->shost, local_mac, ETHADDR_LEN);
  eth->type = htons(ETHTYPE_IP);

  struct ip *ip = (struct ip *)(eth + 1);
  ip->ip_vhl = 0x45; // version 4, header length 4*5
  ip->ip_tos = 0;
  ip->ip_len = htons(sizeof(struct ip) + sizeof(struct udp) + len);
  ip->ip_id = 0;
  ip->ip_off = 0;
  ip->ip_ttl = 100;
  ip->ip_p = IPPROTO_UDP;
  ip->ip_src = htonl(local_ip);
  ip->ip_dst = htonl(dst);
  ip->ip_sum = in_cksum((unsigned char *)ip, sizeof(*ip));

  struct udp *udp = (struct udp *)(ip + 1);
  udp->sport = htons(sport);
  udp->dport = htons(dport);
  udp->ulen = htons(len + sizeof(struct udp));

  char *payload = (char *)(udp + 1);
  if(copyin(p->pagetable, payload, bufaddr, len) < 0){
    kfree(buf);
    printf("send: copyin failed\n");
    return -1;
  }

  e1000_transmit(buf, total);

  return 0;
}

void
ip_rx(char *buf, int len)
{
  // don't delete this printf; make grade depends on it.
  static int seen_ip = 0;
  if(seen_ip == 0)
    printf("ip_rx: received an IP packet\n");
  seen_ip = 1;

  struct eth* eth_hdr = (struct eth*)buf;
  struct ip* ip_hdr = (struct ip*)(eth_hdr + 1);
  struct udp* udp_hdr = (struct udp*)(ip_hdr + 1);

  if (ip_hdr->ip_p != IPPROTO_UDP) {
    kfree(buf);
    return;
  }

  acquire(&utab.lk);
  struct uport* up = find_udp_port(ntohs(udp_hdr->dport));

  if (!up) {
    // no corresponding port
    release(&utab.lk);
    kfree(buf);
    return;
  }

  acquire(&up->lk);
  release(&utab.lk);

  if (up->size >= MAX_PENDING_PACKETS) {
    release(&up->lk);
    kfree(buf);
    return;
  }

  struct upacket* np = kalloc();
  char* payload = kalloc();
  if (!np || !payload) {
    release(&up->lk);
    kfree(buf);
    return;
  }

  uint16 payload_len = ntohs(udp_hdr->ulen) - sizeof(struct udp);
  memmove(payload, (void*)(udp_hdr + 1), payload_len);
  memset((void*)np, 0, sizeof(struct upacket));

  np->buf = (uint64)payload;
  np->len = payload_len;
  np->src_ip = ntohl(ip_hdr->ip_src);
  np->src_port = ntohs(udp_hdr->sport);

  if (!up->size) {
    up->head = np;
    up->tail = np;
    np->next = 0;
  } else {
    up->tail->next = np;
    up->tail = np;
    np->next = 0;
  }
  ++(up->size);
  wakeup(up);
  release(&up->lk);
  kfree(buf);
}

//
// send an ARP reply packet to tell qemu to map
// xv6's ip address to its ethernet address.
// this is the bare minimum needed to persuade
// qemu to send IP packets to xv6; the real ARP
// protocol is more complex.
//
void
arp_rx(char *inbuf)
{
  static int seen_arp = 0;

  if(seen_arp){
    kfree(inbuf);
    return;
  }
  printf("arp_rx: received an ARP packet\n");
  seen_arp = 1;

  struct eth *ineth = (struct eth *) inbuf;
  struct arp *inarp = (struct arp *) (ineth + 1);

  char *buf = kalloc();
  if(buf == 0)
    panic("send_arp_reply");
  
  struct eth *eth = (struct eth *) buf;
  memmove(eth->dhost, ineth->shost, ETHADDR_LEN); // ethernet destination = query source
  memmove(eth->shost, local_mac, ETHADDR_LEN); // ethernet source = xv6's ethernet address
  eth->type = htons(ETHTYPE_ARP);

  struct arp *arp = (struct arp *)(eth + 1);
  arp->hrd = htons(ARP_HRD_ETHER);
  arp->pro = htons(ETHTYPE_IP);
  arp->hln = ETHADDR_LEN;
  arp->pln = sizeof(uint32);
  arp->op = htons(ARP_OP_REPLY);

  memmove(arp->sha, local_mac, ETHADDR_LEN);
  arp->sip = htonl(local_ip);
  memmove(arp->tha, ineth->shost, ETHADDR_LEN);
  arp->tip = inarp->sip;

  e1000_transmit(buf, sizeof(*eth) + sizeof(*arp));

  kfree(inbuf);
}

void
net_rx(char *buf, int len)
{
  struct eth *eth = (struct eth *) buf;

  if(len >= sizeof(struct eth) + sizeof(struct arp) &&
     ntohs(eth->type) == ETHTYPE_ARP){
    arp_rx(buf);
  } else if(len >= sizeof(struct eth) + sizeof(struct ip) &&
     ntohs(eth->type) == ETHTYPE_IP){
    ip_rx(buf, len);
  } else {
    kfree(buf);
  }
}
