// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

#define NSLOT 13

static inline uint hash(uint dev, uint blockno)
{
  return (dev + blockno) % 13;
}

struct slot {
  struct spinlock lock;
  struct buf head;
};

struct {
  // lock that protects bcache.slots when buf are moved between hash slots
  struct spinlock move_lock;
  struct slot slots[NSLOT];
  struct buf buf[NBUF];
} bcache;

void push_front(struct slot* s, struct buf* b)
{
  acquire(&s->lock);
  b->next = s->head.next;
  b->prev = &s->head;
  s->head.next->prev = b;
  s->head.next = b;
  release(&s->lock);
}

void
binit(void)
{
  initlock(&bcache.move_lock, "bcache.move_lock");

  for (int i = 0; i < NSLOT; ++i) {
    initlock(&bcache.slots[i].lock, "bcache.slots.lock");
    bcache.slots[i].head.prev = &bcache.slots[i].head;
    bcache.slots[i].head.next = &bcache.slots[i].head;
  }

  for (int i = 0; i < NBUF; ++i) {
    struct buf* b = &bcache.buf[i];
    initsleeplock(&b->lock, "buffer");
    push_front(&bcache.slots[i % NSLOT], b);
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  const uint key = hash(dev, blockno);
  struct slot* const targetslot = &bcache.slots[key];

  acquire(&targetslot->lock);
  // Is the block already cached?
  for (b = targetslot->head.next; b != &targetslot->head; b = b->next) {
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&targetslot->lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  release(&targetslot->lock);

  // Not cached.
  acquire(&bcache.move_lock);

  // first check if other process added empty buffer to current targetslot
  acquire(&targetslot->lock);
  for (b = targetslot->head.next; b != &targetslot->head; b = b->next) {
    if (b->refcnt == 0) {
      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;
      release(&targetslot->lock);
      release(&bcache.move_lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  release(&targetslot->lock);

  // try to steal from other slot
  for (int i = 0; i < NSLOT; ++i) {
    if (i == key) {
      continue;
    }
    struct slot* victimslot = &bcache.slots[i];
    acquire(&victimslot->lock);
    for (struct buf* b = victimslot->head.next; b != &victimslot->head; b = b->next) {
      if (b->refcnt == 0) {
        b->prev->next = b->next;
        b->next->prev = b->prev;
        push_front(&bcache.slots[key], b);

        b->dev = dev;
        b->blockno = blockno;
        b->valid = 0;
        b->refcnt = 1;

        release(&victimslot->lock);
        release(&bcache.move_lock);
        acquiresleep(&b->lock);
        return b;
      }
    }
    release(&victimslot->lock);
  }
  release(&bcache.move_lock);

  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  struct slot* targetslot = &bcache.slots[hash(b->dev, b->blockno)];
  acquire(&targetslot->lock);
  b->refcnt--;
  release(&targetslot->lock);
}

void
bpin(struct buf *b) {
  struct slot* targetslot = &bcache.slots[hash(b->dev, b->blockno)];
  acquire(&targetslot->lock);
  b->refcnt++;
  release(&targetslot->lock);
}

void
bunpin(struct buf *b) {
  struct slot* targetslot = &bcache.slots[hash(b->dev, b->blockno)];
  acquire(&targetslot->lock);
  b->refcnt--;
  release(&targetslot->lock);
}


