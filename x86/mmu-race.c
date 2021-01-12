#include <libcflat.h>

#include <alloc_page.h>
#include <atomic.h>
#include <desc.h>
#include <processor.h>
#include <smp.h>
#include <vm.h>
#include <vmalloc.h>

static atomic_t write_done;
static atomic_t read_done;
static atomic_t reading;

volatile uint8_t* mema;
pteval_t* mema_pte;

phys_addr_t paddr;
phys_addr_t paddr2;

static pteval_t clear_ad(pteval_t pte) { return pte & ~((1 << 5) | (1 << 6)); }

// Modify a page table entry going back between two versions.
// After writing the page table entry, read it again and check
// that it was not modified.
static void overwrite_pte(void* data) {
  pteval_t v1 = clear_ad(*mema_pte);
  pteval_t v2 = paddr2 | 0x7;
  size_t diffs = 0;

  while (atomic_read(&reading) == 0) {
    asm volatile("pause" ::: "memory");
  }
  for (size_t i = 0; i < 1000000000; i++) {
    *mema_pte = v2;
    pteval_t newe = *mema_pte;
    if (clear_ad(newe) != v2) {
      diffs++;
      printf("Detected overwritten PTE:\n");
      printf("\twant: 0x%016lx\n\tgot:  0x%016lx\n", v2, newe);
      break;
    }
    *mema_pte = v1;
  }

  report(diffs == 0, "PTE not overwritten");
  atomic_set(&write_done, 1);
  while (atomic_read(&read_done) == 0) {
    asm volatile("pause" ::: "memory");
  }
}

// Keep accessing the same virtual address over and over again.
// Until the other cpu tells us to stop.
// This should set the Access bit of the corresponding PTEs.
static void memory_access(void* data) {
  size_t sum = 0;
  atomic_set(&reading, 1);
  while (atomic_read(&write_done) != 1) {
    invlpg(mema);
    sum += mema[0];
  }
  report(sum == 0, "All Reads were zero");
  atomic_set(&read_done, 1);
}

int main(int ac, char** argv) {
  int ncpus;

  setup_vm();

  ncpus = cpu_count();
  printf("found %d cpus\n", ncpus);
  report(ncpus > 1, "Need more than 1 CPU");

  pgd_t* cr3 = current_page_table();

  paddr = virt_to_phys(alloc_page());
  void* second_page = alloc_page();
  memset(second_page, 0x00, PAGE_SIZE);
  paddr2 = virt_to_phys(second_page);

  void* vaddr = (void*)0x000000c800921000ul;

  install_page(cr3, paddr, vaddr);
  memset(vaddr, 0x00, PAGE_SIZE);

  // Get last-level page table entry associated with vaddr.
  pteval_t* l1 = get_pte(cr3, vaddr);

  mema = vaddr;
  mema_pte = l1;

  atomic_set(&reading, 1);
  atomic_set(&write_done, 0);
  atomic_set(&read_done, 0);

  on_cpu_async(1, memory_access, NULL);
  on_cpu(0, overwrite_pte, NULL);

  return report_summary();
}
