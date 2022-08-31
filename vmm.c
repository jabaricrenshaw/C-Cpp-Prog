#include <stdlib.h>		// for exit() 
#include <stdio.h>		// for printf() 
#include <malloc.h>
#include <stdint.h>

/*
**							DATA DECLARATIONS
*/
#define NULL_PTR 	0					// null pointer 
#define BpW	 		4 					// old computer architecture 
#define P_SZ 		1024				// entries per page table, 10 bits to index
#define MAX_PAGES 	P_SZ*P_SZ 			// P_SZ per PT * P_SZ Page Tables
#define P_OFF_MASK 	0xfff				// 1111_1111_1111 page
#define PT_X_MASK 	0x3ff000			// 0011_1111_1111_0000_0000_0000_0000 PT 
#define PD_X_MASK 	0xffc00000			// 1111_1111_1100_0000_0000_0000_0000_0000 PT 
#define TWO32 		0xffffffff			// max integer number -1 
#define BITS22		22					// for count of >>
#define BITS12 		12					// for count of >>

#define FALSE 		0
#define TRUE 		1	
#define NO_VALUE 	-1	// uninitialized

#define BOOL		unsigned	// boolean type 

// Specify the memory attributes of the target machine 
#define CYC_MEM_ACC 		10
#define CYC_PAGE_FAULT_R 	5000
#define CYC_PAGE_FAULT_W	5000

typedef struct page_e_tp {
	int v;			// 4 bytes, int ptr to user-page 
	unsigned lru;	// mem access step of last access
	BOOL dirty;		// BIT clear if no write/modification 
	BOOL present;	// BIT clear if on disk and not in memory 
} struct_page_e_tp;

typedef struct_page_e_tp page_tp[P_SZ];
#define PG_SIZE sizeof(page_tp)

typedef page_tp*page_ptr_tp;

// Page directory, uninitialized 
page_tp pd;

/*
** The FE of the vmm simulator reads the memory requests from stdin. They are
** generated by another program. The FE reads the memory access requests, one 
** at a time, creates triple access, address, and data if applicable
*/
typedef struct triple_tp {
	char action;
	int m_addr;
	int m_val;	// default if action != 'w'
} struct_triple_tp;

#define TRIPLE_SIZE sizeof(struct_triple_tp)

// Triple access, uninitialized 
struct_triple_tp triple;

unsigned access_cnt = 0; 				// num of memory accesses
int triple_cnt = -1;					// account for EPS; num triples 1 greater
unsigned swap_out_cnt = 0;				// times swapped out 
unsigned malloc_page_cnt = 0;			// pages malloced 
unsigned page_fault_cnt = 0;			// count of page faults 

unsigned cycles = 0; 					// number of cycles 
unsigned cycles_no_vmm = 0;				// number of cycles without vmm

unsigned max_phys_pages = 0;			// maximum phyical pages - dictated by max physical mem
unsigned max_pt_pages = 0;				// pages for page tables 
unsigned max_real_pages = 0;			// pages for real data 
unsigned max_log_pages = MAX_PAGES; 	// dictated by architecture 
unsigned ws = 0;						// working set <= max_phys_pages
unsigned max_ws = 0;					// max size of working set 

BOOL want_trace = FALSE; 
BOOL want_swap = FALSE;
BOOL want_dump = FALSE;
int indent = 0;

#define TRACE_CALL
#ifdef TRACE_CALL
void trace_in(char* fu_name) {
	indent++;

	for(int i = 0; i < indent; i++) {
		if ((i % 10) == 0) {
			printf(".");
		} else {
			printf(" ");
		}
	}
	printf(" > %d %s\n", access_cnt, fu_name);
	fflush(stdout);
}

void trace_out(char *fu_name) {
	for (int i = 0; i < indent; i++) {
		if ((i % 10) == 0) {
			printf(".");
		} else {
			printf(" ");
		}
	}
	printf(" < %d %s \n", access_cnt, fu_name);
	indent--;
	fflush(stdout);
}

#else 

#define trace_in 
#define trace_out

#endif

/**
 * General error message 
 */
void error(char *msg) {
	printf("**ERROR** %s. \n", msg);
}

/**
 * Assertion error
 */
void assert(int cond, char *msg) {
	if (!cond) {
		printf("\n * * * Assertion error! * * *\n");
		error(msg);
	}
}

/**
 * Initialize pages - they won't be in use yet, until memory is accessed 
 * due to Demand Paging
 */
void init_page(page_tp page) {
	for (int px = 0; px < P_SZ; px++) {
		page[px].v = NO_VALUE;
		page[px].lru = 0;
		page[px].dirty = FALSE;
		page[px].present = FALSE;
	}
}

// Declare file swap 
FILE *swap;

/**
 * Trace in VM, initialize VM system opening the file swap, 
 * initialize the page, and trace out
 */
void init_vm_system() {
	trace_in("init_vm_system");
	if (!(swap = fopen("swap", "w"))) {
		error("Cannot open file 'swap");
	}
	fprintf(swap, "swap information.\n");

	init_page(pd);
	trace_out("init_vm_system");
} 

/**
 * Place triple access if requested by memory 
 */
void put_triple() {
	if (triple_cnt % 5 == 0) {
		printf("\n");
	}

	printf("%c", triple.action);
	printf("%5d", triple.m_addr);

	if (triple.action == 'w') {
		printf("%4d ", triple.m_val);
	} else {
		printf("      ");
	}
}

/**
 * Get one of the following:
 * 
 * w address value 
 * r address
 * p max_phys_page_frames
 */
int get_triple() {
	char action = ' ';
	int length;
	int value; 

	trace_in("get_triple");
	triple_cnt++;
	while (action == ' ') {
		length = scanf("%s", &action);
		if (length == EOF) {
			trace_out("get_triple");
			return 0;
		} else {
			assert((length == 1), "Wrong number of bytes read");
			switch (action) {
				case 'r':
				case 'w':
				case 'p':
				case ' ':
					break;
				case '\t':
					action = ' ';
					break;
				default: 
					printf("Illegal action. Found '%c'. Only p r w allowed. \n", action);
			}
		}
	}

	triple.action = action;
	length = scanf("%d", &value);
	if (length == EOF) {
		trace_out("get_triple");
		return 0;
	}

	// first int has been read, may be last int
	triple.m_addr = value;
	triple.m_val = NO_VALUE;

	if (action == 'w') {
		length = scanf("%d", &value);
		if (length == EOF) {
			trace_out("get_triple");
			return 0;
		}
		triple.m_val = value;
	} else if (action == 'p') {
		if (0 == max_phys_pages) {
			max_phys_pages = value;
		} else {
			printf("physical pages already set to %d\n", max_phys_pages);
		}
	}
	trace_out("get_triple");
	return 1;
}

/**
 * Get heap space, private function 
 */
char* my_malloc(unsigned size) {
	char *ptr = (char *)malloc(size);

	assert((size % BpW == 0), "Non-aligned memory request");
	assert((ptr != NULL_PTR), "No more heap space");
	return ptr;
}

/**
 * Method to output statistics results 
 */ 
void statistics() {
	printf("\n\n * * * Paging Activity Statistics * * *\n\n");

	printf("number of memory accesses\t= %d\n", access_cnt);
	printf("number of triples (1 + access)\t= %d\n", triple_cnt);
	printf("number of swap ins (faults)\t= %d\n", page_fault_cnt);
	printf("number of swap outs\t\t= %d\n", swap_out_cnt);
	printf("total number of pages malloced\t= %d\n", malloc_page_cnt);
	printf("number of pages for Page Tables\t= %d\n", max_pt_pages);
	printf("number of real pages for user\t= %d\n", max_real_pages);
	printf("total memory cycles\t\t= %d\n", cycles);
	printf("cycles w/o vmm\t\t\t= %d\n", cycles_no_vmm);
	//printf("2 * cycles w/o vmm\t\t\t= %d\n", 2 * cycles_no_vmm);
	printf("cycles per swap_in\t\t= %d\n", CYC_PAGE_FAULT_R);
	printf("cycles per swap_out\t\t= %d\n", CYC_PAGE_FAULT_W);
	printf("last working set size\t\t= %d\n", ws);
	printf("max working set size ever\t= %d\n", max_ws);
	printf("max physical pages\t\t= %d\n", max_phys_pages);
	printf("page size\t\t\t= %d\n", BpW * P_SZ, BpW * P_SZ);
	printf("replacement algorithm\t\t= random");
}

void incrementCycles(int new_cycles) {
	cycles += new_cycles;
}

void show_page_structure(char *msg1, char *msg2) {
	page_ptr_tp pt = NULL_PTR;
	page_ptr_tp p = NULL_PTR; 
	unsigned pdx;
	unsigned ptx;

	trace_in("show_page_structure");
	fprintf(swap, "\n * * * %s %s * * *\n\n", msg1, msg2);

	/**
	 * Sweep through all of the page tables, by running through page directory.
	 * Then sweep through all entries in each page table and search for resident
	 * pages. Find the one with the smallest LRU entry. That one will be swapped 
	 * out. 
	 */

	// Dump page directory. 
	fprintf(swap, "Dumping Page Directory:\n");
	for (pdx = 0; pdx < P_SZ; pdx++) {
		if (pd[pdx].present) {
			pt = (page_ptr_tp) (intptr_t) pd[pdx].v;
			fprintf(swap, "pd[%d].v = 0x%x\n", pdx, (unsigned) pt);
			fprintf(swap, "pd[%d].lru = %d\n", pdx, pd[pdx].lru);
		}
	}
	fflush(swap);

	//Dump page tables 
	fprintf(swap, "\nDumping Page Tables:\n");
	for (pdx = 0; pdx < P_SZ; pdx++) {
		if (pd[pdx].present) {
			pt = (page_ptr_tp) (intptr_t) pd[pdx].v;
			fprintf(swap, "Page Dir entry %d\n", pdx);
			for (ptx = 0; ptx < P_SZ; ptx++) {
				if ((*pt)[ptx].present) {
					p = (page_ptr_tp) (intptr_t) (*pt) [ptx].v;
					fprintf(swap, "pt[%d].v = 0x%07x, ", ptx, (unsigned) p);
					fprintf(swap, "lru = %d", (*pt) [ptx].lru);
					if ((*pt)[ptx].dirty) {
						fprintf(swap, ", written");
					}
					fprintf(swap, "\n");
					fflush(swap);
				}
			}
		}
	}
	fflush(swap);
	trace_out("show_page_structure");
}

void swap_out(char *msg) {
	unsigned lru = TWO32;
	unsigned x = 0;
	unsigned pdx = 0;
	unsigned ptx = 0;
	BOOL initial = TRUE;
	page_ptr_tp pt = NULL_PTR;
	page_ptr_tp ptr;

	trace_in("swap_out");
	swap_out_cnt++;
	assert(ws >= max_phys_pages, "No need to swap out page if room is in working set!");

	/**
	 * Sweep through all page tables by running through each page directory. 
	 * Then sweep through all entries in each page table and search for resident 
	 * pages. Find the one with the smallest LRU entry. That one will be swapped out. 
	 * 
	 * In case we find nothing, initialize the first page to be the page to be swapped
	 * (for good ptr value)
	 */
	ptr = (page_ptr_tp) (intptr_t) pd[0].v; // initial pointer points only to page table frame address

	for (pdx = 0; pdx < P_SZ; pdx++) {
		if (pd[pdx].present) {
			pt = (page_ptr_tp) (intptr_t) pd[pdx].v;
			for (ptx = 0; ptx < P_SZ; ptx++) {
				if ((*pt)[ptx].present) {
					assert((*pt)[ptx].v != NO_VALUE, "pt has no value");

					/**
					 * <= is important cause candidate found may be present. 
					 * while initial ptr is just random. Attributes of page can be swapped 
					 * out.
					 */
					if ((*pt) [ptx].lru <= lru) {
						//remember all data for LRU page 
						initial = FALSE;
						lru = (*pt)[ptx].lru;
						ptr = pt;
						x = ptx;
					} 
				}
			}
		}
	}
	assert(!initial, "Never found a real page to be swapped out.");
	ws--;
	assert(ws < max_phys_pages, "There must be room in the working set");

	// Swap page OUT 
	assert(x >= 0, "X is <= 0");
	assert(x < P_SZ, "X is >= P_SZ");
	assert(ptr != NULL_PTR, "Null ptr");
	assert((*ptr)[x].v > 0, "Null (*ptr)[x].v");
	assert((*ptr)[x].v != NO_VALUE, "(*ptr) [x].v = NO_VALUE");
	assert((*ptr)[x].present, "Cannot swap out if NOT present.");

	(*ptr)[x].present = FALSE;

	/**
	 * ONLY if page is dirty, it must physically be written. Otherwise, 
	 * we can save the write operation since the original page is still on disk. 
	 */
	if ((*ptr)[x].dirty) {
		incrementCycles(CYC_PAGE_FAULT_W);
	}

	if (want_swap) {
		show_page_structure("swapped out a page", msg);
	}
	trace_out("swap_out");
}

/**
 * Swap in
 */
void swap_in(char *msg, page_ptr_tp p_frame, int px) {
	page_ptr_tp ptr = NULL_PTR;
	trace_in("swap_in");
	page_fault_cnt++;
	assert(px >= 0, "px must be >= 0");
	assert(px < P_SZ, "px must be < P_SZ");
	assert(! (*p_frame) [px].present, "resident page cannot be swapped in");
	assert(ws <= max_phys_pages, "Working set size exceeded)");
	if (ws >= max_phys_pages) {
		swap_out("ws full in 'swap_in'; must 'swap_out");
		assert(ws < max_phys_pages, "After swap out no room in working set");
	}
	ws++;

	(*p_frame)[px].present = TRUE;
	(*p_frame)[px].lru = 0;
	(*p_frame)[px].dirty = FALSE;
	if (max_ws < ws) {
		max_ws = ws;
	}
	incrementCycles(CYC_PAGE_FAULT_R);
	if (want_swap) {
		show_page_structure("swapped in a page", msg);
	}
	trace_out("swap_in");
}

void malloc_new_page(BOOL for_pt, char *msg, page_ptr_tp pframe, int px) {
	page_ptr_tp ptr = (page_ptr_tp) my_malloc(PG_SIZE);

	trace_in("malloc new page");
	malloc_page_cnt++;
	if (for_pt) {
		max_pt_pages++;
	} else {
		max_real_pages++;
	} 
	assert((ptr != NULL_PTR), "No space for new page");
	assert((malloc_page_cnt <= max_log_pages), "More pages than logical space");
	init_page((*ptr)); // needs * to make array 

	(*pframe) [px].v = (int) ptr;
	(*pframe) [px].present = TRUE; 
	(*pframe) [px].lru = access_cnt;
	(*pframe) [px].dirty = FALSE; 
	ws++;
	if (max_ws < ws) {
		max_ws = ws;
	}
	if (want_swap) {
		show_page_structure("Done mallocing a new page", msg);
	}
	trace_out("malloc_new_page");
}

void mem_access(struct_triple_tp triple) {
	int la = triple.m_addr;
	int value = triple.m_val;
	char action = triple.action; 

	int pdx; 
	int ptx;
	int poff;
	page_ptr_tp pt;
	page_ptr_tp p;

	trace_in("mem_access");
	access_cnt++;
	cycles_no_vmm += CYC_MEM_ACC;

	assert((action == 'r') || (action == 'w'), "Memory access must be read or write");
	assert((la < TWO32), "Address too high");

	// Get Page Directory index 
	pdx = (la & PD_X_MASK) >> BITS22;
	assert((pdx < P_SZ), "Too high Page Dir index");
	if (! pd[pdx].present) {
		if (pd[pdx].v == NO_VALUE) {
			// page was never swapped into memory 
			malloc_new_page(TRUE, "Page Table", &pd, pdx);
		} else {
			printf("Page-table page is alive but momentarily swapped out");
			swap_in("Page Table", &pd, pdx);
		}
	}
	// now page table is present

	// first cycle count for page table access 
	incrementCycles(CYC_MEM_ACC);
	
	assert(pd[pdx].v > 0, "Page Dir entry must be valid");
	pt = (page_ptr_tp) (intptr_t) pd[pdx].v;
	assert((int) pt > 0, "too low page table address");

	// Get page table index; base we get through page directory access
	ptx = (la & PT_X_MASK) >> BITS12;
	assert((ptx < P_SZ), "Too high page table index");
	if (!(*pt)[ptx].present) {
		if ((*pt)[ptx].v == NO_VALUE) {
			// this real page was never moved into memory 
			if (ws >= max_phys_pages) {
				swap_out("Make room for new real page");
				assert(ws < max_phys_pages, "Must be room in working set");
			}
			malloc_new_page(FALSE, "Malloc real page", pt, ptx);
		} else {
			// real page is alive but momentarily swapped out 
			swap_in("Swap in real page", pt, ptx);
		}
	}

	// Second cycle count for real page access
	incrementCycles(CYC_MEM_ACC);

	// Enter LRU info for page accessed 
	(*pt)[ptx].lru = access_cnt;
	if (action =='w') {
		(*pt)[ptx].dirty = TRUE; 
	}

	p = (page_ptr_tp) (intptr_t) (*pt)[ptx].v;
	assert((int) (*p) > 0, "too low page table address");
	assert((int) (*p) < TWO32, "too high page directory address");

	/**
	 * Get page offset; 10 bits, beacuse will not be scaled by 4
	 * 
	 * Not needed? hello?
	 */
	poff = la & P_OFF_MASK;
	assert((poff < (P_SZ << 2)), "too high page offset");
	assert((poff % BpW == 0), "We only deal with word addresses");
	trace_out("mem_access");
}

void all_mem_accesses() {
	(void) get_triple();
	assert((max_phys_pages > 0), "No page number info given");
	if (want_trace) {
		put_triple();
	}

	while (get_triple()) {
		mem_access(triple);
		if (want_trace) {
			put_triple();
		}
	}

	if (want_trace) {
		printf("\n");
	}
}

void set_switches(int argc, char *argv[]) {
	int i = 1;
	if (1 == argc) {
		printf("usage is: vmm [-t trace] [-d dump] [-s swap] < in_file [> out_file].\n");
		exit(0);
	} else {
		while (argc > i) {
			assert((argv[i][0] == '-'), "introduce switch with '-'");
			switch(argv[i][1]) {
				case 't':
					want_trace = TRUE; 
					break;
				case 'd':
					want_dump = TRUE;
					break; 
				case 's': 
					want_swap = TRUE; 
					break;
				default: 
					printf("usage is: vmm [-t trace] [-d dump] [-s swap] < in_file [> out_file].\n");	
			}
			i++;
		}
	}
}

int main(int argc, char *argv[]) {
	set_switches(argc, argv);
	init_vm_system();
	all_mem_accesses();
	statistics();
	return 0;
}