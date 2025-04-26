//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  argint(n, &fd);
  if(fd < 0 || fd >= NOFILE || (f=myproc()->ofile[fd]) == 0)
    return -1;
  if(pfd)
    *pfd = fd;
  if(pf)
    *pf = f;
  return 0;
}

// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
static int
fdalloc(struct file *f)
{
  int fd;
  struct proc *p = myproc();

  for(fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd] == 0){
      p->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

uint64
sys_dup(void)
{
  struct file *f;
  int fd;

  if(argfd(0, 0, &f) < 0)
    return -1;
  if((fd=fdalloc(f)) < 0)
    return -1;
  filedup(f);
  return fd;
}

uint64
sys_read(void)
{
  struct file *f;
  int n;
  uint64 p;

  argaddr(1, &p);
  argint(2, &n);
  if(argfd(0, 0, &f) < 0)
    return -1;
  return fileread(f, p, n);
}

uint64
sys_write(void)
{
  struct file *f;
  int n;
  uint64 p;
  
  argaddr(1, &p);
  argint(2, &n);
  if(argfd(0, 0, &f) < 0)
    return -1;

  return filewrite(f, p, n);
}

uint64
sys_close(void)
{
  int fd;
  struct file *f;

  if(argfd(0, &fd, &f) < 0)
    return -1;
  myproc()->ofile[fd] = 0;
  fileclose(f);
  return 0;
}

uint64
sys_fstat(void)
{
  struct file *f;
  uint64 st; // user pointer to struct stat

  argaddr(1, &st);
  if(argfd(0, 0, &f) < 0)
    return -1;
  return filestat(f, st);
}

// Create the path new as a link to the same inode as old.
uint64
sys_link(void)
{
  char name[DIRSIZ], new[MAXPATH], old[MAXPATH];
  struct inode *dp, *ip;

  if(argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
    return -1;

  begin_op();
  if((ip = namei(old)) == 0){
    end_op();
    return -1;
  }

  ilock(ip);
  if(ip->type == T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }

  ip->nlink++;
  iupdate(ip);
  iunlock(ip);

  if((dp = nameiparent(new, name)) == 0)
    goto bad;
  ilock(dp);
  if(dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0){
    iunlockput(dp);
    goto bad;
  }
  iunlockput(dp);
  iput(ip);

  end_op();

  return 0;

bad:
  ilock(ip);
  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);
  end_op();
  return -1;
}

// Is the directory dp empty except for "." and ".." ?
static int
isdirempty(struct inode *dp)
{
  int off;
  struct dirent de;

  for(off=2*sizeof(de); off<dp->size; off+=sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("isdirempty: readi");
    if(de.inum != 0)
      return 0;
  }
  return 1;
}

uint64
sys_unlink(void)
{
  struct inode *ip, *dp;
  struct dirent de;
  char name[DIRSIZ], path[MAXPATH];
  uint off;

  if(argstr(0, path, MAXPATH) < 0)
    return -1;

  begin_op();
  if((dp = nameiparent(path, name)) == 0){
    end_op();
    return -1;
  }

  ilock(dp);

  // Cannot unlink "." or "..".
  if(namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    goto bad;

  if((ip = dirlookup(dp, name, &off)) == 0)
    goto bad;
  ilock(ip);

  if(ip->nlink < 1)
    panic("unlink: nlink < 1");
  if(ip->type == T_DIR && !isdirempty(ip)){
    iunlockput(ip);
    goto bad;
  }

  memset(&de, 0, sizeof(de));
  if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    panic("unlink: writei");
  if(ip->type == T_DIR){
    dp->nlink--;
    iupdate(dp);
  }
  iunlockput(dp);

  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);

  end_op();

  return 0;

bad:
  iunlockput(dp);
  end_op();
  return -1;
}

static struct inode*
create(char *path, short type, short major, short minor)
{
  struct inode *ip, *dp;
  char name[DIRSIZ];

  if((dp = nameiparent(path, name)) == 0)
    return 0;

  ilock(dp);

  if((ip = dirlookup(dp, name, 0)) != 0){
    iunlockput(dp);
    ilock(ip);
    if(type == T_FILE && (ip->type == T_FILE || ip->type == T_DEVICE))
      return ip;
    iunlockput(ip);
    return 0;
  }

  if((ip = ialloc(dp->dev, type)) == 0){
    iunlockput(dp);
    return 0;
  }

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  if(type == T_DIR){  // Create . and .. entries.
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if(dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      goto fail;
  }

  if(dirlink(dp, name, ip->inum) < 0)
    goto fail;

  if(type == T_DIR){
    // now that success is guaranteed:
    dp->nlink++;  // for ".."
    iupdate(dp);
  }

  iunlockput(dp);

  return ip;

 fail:
  // something went wrong. de-allocate ip.
  ip->nlink = 0;
  iupdate(ip);
  iunlockput(ip);
  iunlockput(dp);
  return 0;
}

uint64
sys_open(void)
{
  char path[MAXPATH];
  int fd, omode;
  struct file *f;
  struct inode *ip;
  int n;

  argint(1, &omode);
  if((n = argstr(0, path, MAXPATH)) < 0)
    return -1;

  begin_op();

  if(omode & O_CREATE){
    ip = create(path, T_FILE, 0, 0);
    if(ip == 0){
      end_op();
      return -1;
    }
  } else {
    if((ip = namei(path)) == 0){
      end_op();
      return -1;
    }
    ilock(ip);
    if(ip->type == T_DIR && omode != O_RDONLY){
      iunlockput(ip);
      end_op();
      return -1;
    }
  }

  if(ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV)){
    iunlockput(ip);
    end_op();
    return -1;
  }

  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
    if(f)
      fileclose(f);
    iunlockput(ip);
    end_op();
    return -1;
  }

  if(ip->type == T_DEVICE){
    f->type = FD_DEVICE;
    f->major = ip->major;
  } else {
    f->type = FD_INODE;
    f->off = 0;
  }
  f->ip = ip;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);

  if((omode & O_TRUNC) && ip->type == T_FILE){
    itrunc(ip);
  }

  iunlock(ip);
  end_op();

  return fd;
}

uint64
sys_mkdir(void)
{
  char path[MAXPATH];
  struct inode *ip;

  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = create(path, T_DIR, 0, 0)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_mknod(void)
{
  struct inode *ip;
  char path[MAXPATH];
  int major, minor;

  begin_op();
  argint(1, &major);
  argint(2, &minor);
  if((argstr(0, path, MAXPATH)) < 0 ||
     (ip = create(path, T_DEVICE, major, minor)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_chdir(void)
{
  char path[MAXPATH];
  struct inode *ip;
  struct proc *p = myproc();
  
  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);
  if(ip->type != T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  iput(p->cwd);
  end_op();
  p->cwd = ip;
  return 0;
}

uint64
sys_exec(void)
{
  char path[MAXPATH], *argv[MAXARG];
  int i;
  uint64 uargv, uarg;

  argaddr(1, &uargv);
  if(argstr(0, path, MAXPATH) < 0) {
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for(i=0;; i++){
    if(i >= NELEM(argv)){
      goto bad;
    }
    if(fetchaddr(uargv+sizeof(uint64)*i, (uint64*)&uarg) < 0){
      goto bad;
    }
    if(uarg == 0){
      argv[i] = 0;
      break;
    }
    argv[i] = kalloc();
    if(argv[i] == 0)
      goto bad;
    if(fetchstr(uarg, argv[i], PGSIZE) < 0)
      goto bad;
  }

  int ret = exec(path, argv);

  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);

  return ret;

 bad:
  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);
  return -1;
}

uint64
sys_pipe(void)
{
  uint64 fdarray; // user pointer to array of two integers
  struct file *rf, *wf;
  int fd0, fd1;
  struct proc *p = myproc();

  argaddr(0, &fdarray);
  if(pipealloc(&rf, &wf) < 0)
    return -1;
  fd0 = -1;
  if((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0){
    if(fd0 >= 0)
      p->ofile[fd0] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  if(copyout(p->pagetable, fdarray, (char*)&fd0, sizeof(fd0)) < 0 ||
     copyout(p->pagetable, fdarray+sizeof(fd0), (char *)&fd1, sizeof(fd1)) < 0){
    p->ofile[fd0] = 0;
    p->ofile[fd1] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  return 0;
}

// sys_mmap: create a new anonymous or file‑backed mapping at the end
// of the process’s address space.
//   addr:   hint (ignored; mapping is always placed at p->sz)
//   len:    requested size in bytes
//   prot:   protection flags (PROT_READ, PROT_WRITE, …)
//   flags:  MAP_SHARED or MAP_PRIVATE
//   fd:     file descriptor (for shared/private file mappings)
//   offset: offset within file to map
//
// For MAP_SHARED+PROT_WRITE, the file must be open writable.
// On success: allocates a VMA, rounds len to pages, extends p->sz,
//               and returns the new mapping’s start address.
// On failure: returns –1.
uint64 sys_mmap(void)
{
  uint64 addr;
  int len;
  int prot;
  int flags;
  int fd;
  struct file* f;
  int offset;
  argaddr(0, &addr);
  argint(1, &len);
  argint(2, &prot);
  argint(3, &flags);
  if (argfd(4, &fd, &f) < 0) {
    return -1;
  }
  argint(5, &offset);

  if (flags == MAP_SHARED && !f->writable && (prot & PROT_WRITE)) {
    return -1;
  }

  struct proc* p = myproc();
  for (int i = 0; i < MAXVMA; ++i) {
    struct vma* v = &(p->vma_array[i]);
    if (!v->valid) {
      v->valid = 1;
      v->start = p->sz;
      v->len = len;
      v->protection = prot;
      v->flags = flags;
      v->offset = offset;
      v->file = filedup(f);
      len = PGROUNDUP(len);
      p->sz += len;
      v->end = p->sz;
      return v->start;
    }
  }

  return -1;
}

// sys_munmap: unmap [addr, addr+length) from the calling process’s address space.
// - Rounds addr down and length up to full pages.
// - Finds the VMA containing the start page.
// - If MAP_SHARED, writes back any unmapped pages to the file.
// - Removes pages from the page table in page‑sized chunks.
// - Adjusts or frees the VMA accordingly.
// Returns 0 on success.
uint64 sys_munmap(void)
{
  uint64 addr;
  int length;
  int npages;
  struct proc* p = myproc();

  argaddr(0, &addr);
  argint(1, &length);

  uint64 a = PGROUNDDOWN(addr);
  int len = PGROUNDUP(length);
  npages = len / PGSIZE;

  for (int i = 0; i < MAXVMA; i++) {
    struct vma* v = &p->vma_array[i];
    if (!v->valid || a < v->start || a >= v->end) {
      continue;
    }

    if (a == v->start) {
      if (len >= v->len) {
        // whole VMA is being unmapped
        if (v->flags & MAP_SHARED) {
          filewrite_offset(v->file, v->start, v->len, v->offset);
        }
        uvmunmap(p->pagetable, v->start, v->len / PGSIZE, 1);
        fileclose(v->file);
        v->valid = 0;
        v->file = 0;
        v->start = 0;
        v->end = 0;
        v->len = 0;
        v->offset = 0;
        v->protection = 0;
        v->flags = 0;
      } else {
        // only the first part of the VMA
        if (v->flags & MAP_SHARED) {
          filewrite_offset(v->file, v->start, len, v->offset);
        }
        uvmunmap(p->pagetable, v->start, npages, 1);
        v->start += len;
        v->offset += len;
        v->len -= len;
        v->end = v->start + v->len;
      }
    } else {
      // middle of the VMA
      if (v->flags & MAP_SHARED) {
        filewrite_offset(v->file, a, len, v->offset);
      }
      uvmunmap(p->pagetable, a, npages, 1);
      v->len -= len;
      v->end = v->start + v->len;
    }
  }

  return 0;
}

// lazy mapping the memory when page fault.
int map_mmap(struct proc* p, uint64 va_fault)
{
  for (int i = 0; i < MAXVMA; i++) {
    struct vma* v = &p->vma_array[i];

    if (!v->valid) {
      continue;
    }
    if (va_fault < v->end && va_fault >= v->start) {
      uint64 va_page = PGROUNDDOWN(va_fault);
      uint64 file_off = va_page - v->start + v->offset;

      char* kva = kalloc();
      if (kva == 0) {
        return -1;
      }
      memset(kva, 0, PGSIZE);

      uint64 pa = (uint64)kva;
      uint flags = (v->protection << 1) | PTE_U | PTE_A | PTE_D;

      if (mappages(p->pagetable, va_page, PGSIZE, pa, flags) != 0) {
        kfree(kva);
        return -1;
      }

      ilock(v->file->ip);
      if (readi(v->file->ip, 0, (uint64)kva, file_off, PGSIZE) < 0) {
        iunlock(v->file->ip);
        kfree(kva);
        uvmunmap(p->pagetable, va_page, 1, 0);
        return -1;
      }
      iunlock(v->file->ip);
      return 0;
    }
  }
  return -1;
}

// Write up to `n` bytes from user buffer at virtual address `addr`
// into file `f`, starting at file‑offset `offset`.
// Returns the number of bytes written (== n) on success, or -1 on error.
int filewrite_offset(struct file* f, uint64 addr, int n, int offset)
{
  if (f->writable == 0 || f->type != FD_INODE) {
    return -1;
  }

  // current file size
  int remain = f->ip->size - offset;
  if (remain <= 0) { // offset is beyond EOF
    return -1;
  }
  if (n > remain) { // don't write past EOF
    n = remain;
  }

  int max = ((MAXOPBLOCKS - 1 - 1 - 2) / 2) * BSIZE;
  int i = 0;
  int r;

  while (i < n) {
    int n1 = n - i;
    if (n1 > max) {
      n1 = max;
    }

    begin_op();
    ilock(f->ip);
    r = writei(f->ip, 1 /*USER buf*/, addr + i, offset, n1);
    iunlock(f->ip);
    end_op();

    if (r != n1) {
      break;
    }
    offset += r;
    i += r;
  }
  return (i == n) ? n : -1;
}
