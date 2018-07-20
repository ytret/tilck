
#include <exos/common/basic_defs.h>
#include <exos/common/string_util.h>
#include <exos/common/utils.h>

#include <exos/kernel/paging.h>
#include <exos/kernel/sort.h>
#include <exos/kernel/elf_utils.h>

#include <multiboot.h>


#define MEM_REG_EXTRA_RAMDISK  1
#define MEM_REG_EXTRA_KERNEL   2
#define MEM_REG_EXTRA_LOWMEM   4

typedef struct {

   u64 addr;
   u64 len;
   u32 type;  /* multiboot_memory_map_t's type */
   u32 extra; /* bit mask */

} memory_region_t;

u32 memsize_in_mb;

memory_region_t mem_regions[512];
int mem_regions_count;

uptr ramdisk_paddr;
size_t ramdisk_size;

STATIC int less_than_cmp_mem_region(const void *a, const void *b)
{
   const memory_region_t *m1 = a;
   const memory_region_t *m2 = b;

   if (m1->addr < m2->addr)
      return -1;

   if (m1->addr == m2->addr)
      return 0;

   return 1;
}

STATIC void append_mem_region(memory_region_t r)
{
   if (mem_regions_count >= (int)ARRAY_SIZE(mem_regions))
      panic("Too many memory regions (limit: %u)", ARRAY_SIZE(mem_regions));

   mem_regions[mem_regions_count++] = r;
}

STATIC void remove_mem_region(int i)
{
   memory_region_t *ma = mem_regions + i;
   const int rem = mem_regions_count - i - 1;

   memcpy(ma, ma + 1, rem * sizeof(memory_region_t));
   mem_regions_count--; /* decrease the number of memory regions */
}

STATIC void align_mem_regions_to_page_boundary(void)
{
   for (int i = 0; i < mem_regions_count; i++) {

      memory_region_t *ma = mem_regions + i;

      /*
       * Unfortunately, in general we cannot rely on the memory regions to be
       * page-aligned (while they will be in most of the cases). Therefore,
       * we have to forcibly approximate the regions at page-boundaries.
       */
      const u64 ma_end = round_up_at64(ma->addr + ma->len, PAGE_SIZE);
      ma->addr &= PAGE_MASK;
      ma->len = ma_end - ma->addr;
   }
}

STATIC void merge_adj_mem_regions(void)
{
   for (int i = 0; i < mem_regions_count - 1; i++) {

      memory_region_t *ma = mem_regions + i;
      memory_region_t *ma_next = ma + 1;

      if (ma_next->type != ma->type || ma_next->extra != ma->extra)
         continue;

      if (ma_next->addr != ma->addr + ma->len)
         continue;

      /* If we got here, we hit two adjacent regions having the same type */

      ma->len += ma_next->len;
      remove_mem_region(i + 1);
      i--; /* compensate the i++ in the for loop: we have to keep the index */
   }
}

STATIC void handle_region_overlap(int r1_index, int r2_index)
{
   if (r1_index == r2_index)
      return;

   memory_region_t *r1 = mem_regions + r1_index;
   memory_region_t *r2 = mem_regions + r2_index;

   u64 s1 = r1->addr;
   u64 s2 = r2->addr;
   u64 e1 = r1->addr + r1->len;
   u64 e2 = r2->addr + r2->len;

   if (s2 < s1 || s2 >= e1) {

      /*
       * Skip the following cases:
       *
       * Case 1a:
       *                         +------------------------+
       *                         |        region 1        |
       *                         +------------------------+
       *  +----------------------+
       *  |       region 2       |
       *  +----------------------+
       *
       * Reason: the regions do not overlap.
       *
       * Case 1b:
       *
       *                +------------------------+
       *                |        region 1        |
       *                +------------------------+
       *  +----------------------+
       *  |       region 2       |
       *  +----------------------+
       *
       * Reason: we'll handle the case when i and j are swapped (see case 5).
       *
       * Case 1c:
       *
       *  +----------------------+
       *  |       region 1       |
       *  +----------------------+
       *                         +----------------------+
       *                         |       region 2       |
       *                         +----------------------+
       *
       * Reason: the regions do not overlap.
       */
      return;
   }


   if (s1 <= s2 && e2 <= e1) {

      /*
       * Case 2:
       *  +---------------------------------+
       *  |            region 1             |
       *  +---------------------------------+
       *            +---------------+
       *            |   region 2    |
       *            +---------------+
       *
       * Corner case 2a:
       *  +---------------------------------+
       *  |            region 1             |
       *  +---------------------------------+
       *  +---------------------------------+
       *  |            region 2             |
       *  +---------------------------------+
       *
       * Corner case 2b:
       *  +---------------------------------+
       *  |            region 1             |
       *  +---------------------------------+
       *  +---------------+
       *  |   region 2    |
       *  +---------------+
       *
       * Corner case 2c:
       *  +---------------------------------+
       *  |            region 1             |
       *  +---------------------------------+
       *                    +---------------+
       *                    |   region 2    |
       *                    +---------------+
       */

      if (r1->type >= r2->type) {

         /*
          * Region 1's type is stricter than region 2's. Just remove region 2.
          */

         remove_mem_region(r2_index);

      } else {

         /*
          * Region 2's type is stricter, we need to split region 1 in two parts:
          *  +---------------+               +-------------------+
          *  |  region 1-1   |               |     region 1-2    |
          *  +---------------+               +-------------------+
          *                  +---------------+
          *                  |   region 2    |
          *                  +---------------+
          */

         if (s1 == s2 && e1 == e2) {

            /*
             * Corner case 2a: regions 1-1, 1-2 are empty, but we cannot remove
             * region 1: make region 1 = region 2 and remove region 2.
             */

            *r1 = *r2;
            remove_mem_region(r2_index);

         } else if (s1 == s2) {

            /* Corner case 2b: "region 1 [1]" is empty" */

            r1->addr = e2;
            r1->len = e1 - r1->addr;

         } else if (e1 == e2) {

            /* Corner case 2c: "region 1 [2]" is empty" */

            r1->len = s2 - s1;

         } else {

            /* Base case */

            r1->len = s2 - s1;

            append_mem_region((memory_region_t) {
               .addr = e2,
               .len = (e1 - e2),
               .type = r1->type,
               .extra = r1->extra
            });
         }
      }

      return;
   }

   if (s1 <= s2 && s2 < e1 && e2 > e1) {

      /*
       * Case 3:
       *  +---------------------------------+
       *  |            region 1             |
       *  +---------------------------------+
       *                    +---------------------------+
       *                    |          region 2         |
       *                    +---------------------------+
       *
       * Corner case 3a:
       *
       *  +----------------------------+
       *  |          region 1          |
       *  +----------------------------+
       *  +--------------------------------------------+
       *  |                  region 2                  |
       *  +--------------------------------------------+
       */

      if (r1->type >= r2->type) {

         /*
          * Region 1's type is stricter than region 2's. Move region 2's start.
          *  +---------------------------------+
          *  |            region 1             |
          *  +---------------------------------+
          *                                    +-----------+
          *                                    |  region 2 |
          *                                    +-----------+
          */

         r2->addr = e1;
         r2->len = e2 - r2->addr;

      } else {

         /*
          * Region 2's type is stricter, move region 1's end.
          *
          *  +-----------------+
          *  |    region 1     |
          *  +-----------------+
          *                    +---------------------------+
          *                    |          region 2         |
          *                    +---------------------------+
          */

         if (s1 == s2) {
            /* Corner case 3a: region 1 is empty */
            *r1 = *r2;
            remove_mem_region(r2_index);
         } else {
            r1->len = s2 - s1;
         }
      }

      return;
   }

   /*
    * There should NOT be any unhandled cases.
    */
   NOT_REACHED();
}

STATIC void handle_overlapping_regions(void)
{
   for (int i = 0; i < mem_regions_count; i++)
      for (int j = 0; j < mem_regions_count; j++)
         handle_region_overlap(i, j);
}

STATIC void fix_mem_regions(void)
{
   align_mem_regions_to_page_boundary();

   insertion_sort_generic(mem_regions,
                          sizeof(memory_region_t),
                          mem_regions_count,
                          less_than_cmp_mem_region);

   merge_adj_mem_regions();
   handle_overlapping_regions();

   insertion_sort_generic(mem_regions,
                          sizeof(memory_region_t),
                          mem_regions_count,
                          less_than_cmp_mem_region);
}

STATIC void add_kernel_phdrs_to_mmap(void)
{
   Elf_Ehdr *h = (Elf_Ehdr*)(KERNEL_PA_TO_VA(KERNEL_PADDR));
   Elf_Phdr *phdrs = (void *)h + h->e_phoff;

   for (int i = 0; i < h->e_phnum; i++) {

      Elf_Phdr *phdr = phdrs + i;

      if (phdr->p_type != PT_LOAD)
         continue;

      append_mem_region((memory_region_t) {
         .addr = phdr->p_paddr,
         .len = phdr->p_memsz,
         .type = MULTIBOOT_MEMORY_RESERVED,
         .extra = MEM_REG_EXTRA_KERNEL
      });
   }
}

void save_multiboot_memory_map(multiboot_info_t *mbi)
{
   uptr ma_addr = mbi->mmap_addr;

   /* We want to keep the first 64 KB as reserved */
   append_mem_region((memory_region_t) {
      .addr = 0,
      .len = 64 * KB,
      .type = MULTIBOOT_MEMORY_RESERVED,
      .extra = MEM_REG_EXTRA_LOWMEM
   });

   while (ma_addr < mbi->mmap_addr + mbi->mmap_length) {

      multiboot_memory_map_t *ma = (void *)ma_addr;
      append_mem_region((memory_region_t) {
         .addr = ma->addr,
         .len = ma->len,
         .type = ma->type,
         .extra = 0
      });
      ma_addr += ma->size + 4;
   }

   if (ramdisk_size) {
      append_mem_region((memory_region_t) {
         .addr = ramdisk_paddr,
         .len = ramdisk_size,
         .type = MULTIBOOT_MEMORY_RESERVED,
         .extra = MEM_REG_EXTRA_RAMDISK
      });
   }

   add_kernel_phdrs_to_mmap();
   fix_mem_regions();
}

static const char *mem_region_extra_to_str(u32 e)
{
   if (e == MEM_REG_EXTRA_RAMDISK)
      return "RDSK";

   if (e == MEM_REG_EXTRA_KERNEL)
      return "KRNL";

   if (e == MEM_REG_EXTRA_LOWMEM)
      return "LMRS";

   return "    ";
}

void dump_system_memory_map(void)
{
   printk("System's memory map\n");
   printk("---------------------------------------------------------------\n");
   printk("       START                 END        (T, Extr)\n");
   for (int i = 0; i < mem_regions_count; i++) {

      memory_region_t *ma = mem_regions + i;

      printk("0x%llx - 0x%llx (%d, %s) [%u KB]\n",
             ma->addr, ma->addr + ma->len,
             ma->type, mem_region_extra_to_str(ma->extra), ma->len / KB);
   }
   printk("---------------------------------------------------------------\n");
}
