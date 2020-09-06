#include "kernel.hh"
#include "k-apic.hh"
#include "k-vmiter.hh"
#include <atomic>

// kernel.cc
//
//    This is the kernel.


// INITIAL PHYSICAL MEMORY LAYOUT
//
//  +-------------- Base Memory --------------+
//  v                                         v
// +-----+--------------------+----------------+--------------------+---------/
// |     | Kernel      Kernel |       :    I/O | App 1        App 1 | App 2
// |     | Code + Data  Stack |  ...  : Memory | Code + Data  Stack | Code ...
// +-----+--------------------+----------------+--------------------+---------/
// 0  0x40000              0x80000 0xA0000 0x100000             0x140000
//                                             ^
//                                             | \___ PROC_SIZE ___/
//                                      PROC_START_ADDR

#define PROC_SIZE 0x40000       // initial state only

proc ptable[NPROC];             // array of process descriptors
                                // Note that `ptable[0]` is never used.
proc* current;                  // pointer to currently executing proc

#define HZ 100                  // timer interrupt frequency (interrupts/sec)
static std::atomic<unsigned long> ticks; // # timer interrupts so far


// Memory state
//    Information about physical page with address `pa` is stored in
//    `pages[pa / PAGESIZE]`. In the handout code, each `pages` entry
//    holds an `refcount` member, which is 0 for free pages.
//    You can change this as you see fit.

pageinfo pages[NPAGES];


[[noreturn]] void schedule();
[[noreturn]] void run(proc* p);
void exception(regstate* regs);
uintptr_t syscall(regstate* regs);
void memshow();
int fork();
void sys_exit();
uintptr_t should_free[NPAGES];

// kernel(command)
//    Initialize the hardware and processes and start running. The `command`
//    string is an optional string passed from the boot loader.

static void process_setup(pid_t pid, const char* program_name);

void kernel(const char* command) {
    // initialize hardware
    init_hardware();
    log_printf("Starting WeensyOS\n");

    ticks = 1;
    init_timer(HZ);

    // clear screen
    console_clear();

    // (re-)initialize kernel page table
    for (vmiter it(kernel_pagetable);
         it.va() < MEMSIZE_PHYSICAL;
         it += PAGESIZE) {
        if (it.va() != 0) {
		//Isolate the kernel without the console:                                        details
		if (it.va()>=PROC_START_ADDR || it.va()==CONSOLE_ADDR)
            		it.map(it.va(), PTE_P | PTE_W | PTE_U);
		else
			it.map(it.va(), PTE_P | PTE_W);
        } else {
            // nullptr is inaccessible even to the kernel
            it.map(it.va(), 0);
        }
    }

    // set up process descriptors
    for (pid_t i = 0; i < NPROC; i++) {
        ptable[i].pid = i;
        ptable[i].state = P_FREE;
    }
    if (command && program_loader(command).present()) {
        process_setup(1, command);
    } else {
        process_setup(1, "allocator");
        process_setup(2, "allocator2");
        process_setup(3, "allocator3");
        process_setup(4, "allocator4");
    }

    // Switch to the first process using run()
    run(&ptable[1]);
}


// kalloc(sz)
//    Kernel memory allocator. Allocates `sz` contiguous bytes and
//    returns a pointer to the allocated memory, or `nullptr` on failure.
//
//    The returned memory is initialized to 0xCC, which corresponds to
//    the x86 instruction `int3` (this may help you debug). You'll
//    probably want to reset it to something more useful.
//
//    On WeensyOS, `kalloc` is a page-based allocator: if `sz > PAGESIZE`
//    the allocation fails; if `sz < PAGESIZE` it allocates a whole page
//    anyway.
//
//    The handout code returns the next allocatable free page it can find.
//    It never reuses pages or supports freeing memory (you'll change that).

static uintptr_t next_alloc_pa;

void* kalloc(size_t sz) {
    if (sz > PAGESIZE) {
        return nullptr;
    }
    while (next_alloc_pa < MEMSIZE_PHYSICAL) {
        uintptr_t pa = next_alloc_pa;
        next_alloc_pa += PAGESIZE;

        if (allocatable_physical_address(pa)
            && !pages[pa / PAGESIZE].used()) {
            pages[pa / PAGESIZE].refcount = 1;
            memset((void*) pa, 0, PAGESIZE); //initialize it here so you don't have to initialize it everywhere else
            return (void*) pa;
        }
    }
    return nullptr;
}


// kfree(kptr)
//    Free `kptr`, which must have been previously returned by `kalloc`.
//    If `kptr == nullptr` does nothing.

void kfree(void* kptr) {
    (void) kptr;
	if (kptr==nullptr) return;

    //change the next_alloc_pa so that kalloc can reuse this page:
    if (next_alloc_pa>(uintptr_t)kptr && allocatable_physical_address((uintptr_t)kptr))
    {
            next_alloc_pa=(uintptr_t)kptr;
    }
    //mark that it is not used anymore:
    pages[(uintptr_t)kptr / PAGESIZE].refcount=0;


    return;
}


// arrfree(p,index)
//   arrfree frees all the pages pointed to by p
//   index is the size of p

void arrfree(uintptr_t* p,int index){
	index--;
	while (index>=0){
		kfree((void*)p[index]);
		index--;
	}
	return;
}

// process_setup(pid, program_name)
//    Load application program `program_name` as process number `pid`.
//    This loads the application's code and data into memory, sets its
//    %rip and %rsp, gives it a stack page, and marks it as runnable.

void process_setup(pid_t pid, const char* program_name) {
    init_process(&ptable[pid], 0);

    //This is used to keep track of the should_free array:
    unsigned findex=0;

    //Allocate a new page table for the process:
    x86_64_pagetable* new_pagetable;
    new_pagetable=(x86_64_pagetable*) kalloc(PAGESIZE);

    //What if there is no space?
    if (new_pagetable==nullptr){
	return;
    }

    //If we successfully kalloced a page we should keep track of it:
    should_free[findex]=(uintptr_t)new_pagetable;
    findex++;

    //Copy the kernel's mappings:
    vmiter newit(new_pagetable,0);   //iterates over the new pagetable
    vmiter kerit(kernel_pagetable,0);//iterates over kalloc's pagetable

    while (kerit.va()<MEMSIZE_VIRTUAL){
        if (kerit.pa()<PROC_START_ADDR && kerit.present()){
            int check = newit.try_map(kerit.pa(),kerit.perm());
            if (check<0){
                //If you go into an error, clean up before leaving
                //arrfree frees everything kalloced until now
                arrfree(should_free,findex);
                return;
            }
        }
        newit+=PAGESIZE;
        kerit+=PAGESIZE;
    }

    // load the program
    program_loader loader(program_name);

    // initialize process page table
    ptable[pid].pagetable = new_pagetable;


    // allocate and map all memory
    for (loader.reset(); loader.present(); ++loader) {
        for (uintptr_t a = round_down(loader.va(), PAGESIZE);
             a < loader.va() + loader.size();
             a += PAGESIZE) {
            //Allocate new pages to load into:
            void* paddr=kalloc(PAGESIZE);
            //if kalloc failed clean up:
            if (paddr==nullptr){
                arrfree(should_free,findex);
                return;
            }
            //Keep track:
            should_free[findex]=(uintptr_t)paddr;
            findex++;
            // check is for try map
            int check=0;
            // mark read only memory:
            if (loader.writable()){
                check = vmiter(new_pagetable,a).try_map(paddr,PTE_P | PTE_W | PTE_U );
            }
            else{
                check = vmiter(new_pagetable,a).try_map(paddr,PTE_P | PTE_U );
            }
            //If one of the try maps fail you should clean up:
            if (check<0){
                arrfree(should_free,findex);
                return;
            }
            }
    }

    // copy instructions and data into place:
    for (loader.reset(); loader.present(); ++loader) {
         for (uintptr_t a = round_down(loader.va(), PAGESIZE);
             a < loader.va() + loader.size();
             a += PAGESIZE) {
                memset((void*) vmiter(new_pagetable,loader.va()).pa(), 0, loader.size());
                memcpy((void*) vmiter(new_pagetable,loader.va()).pa(), loader.data(), loader.data_size());
            }
    }

    // mark entry point
    ptable[pid].regs.reg_rip = loader.entry();

    // allocate stack:
	//Find the address and kalloc a page:
	uintptr_t stack_addr = MEMSIZE_VIRTUAL-PAGESIZE;
	void* phy_stack=kalloc(PAGESIZE);
	//if kalloc fail clean up:
	if (phy_stack==nullptr){
		arrfree(should_free,findex);
		return;
	}
	//Keep track:
	should_free[findex]=(uintptr_t)phy_stack;
	findex++;
    //map the stack
    ptable[pid].regs.reg_rsp = stack_addr + PAGESIZE;
	int check = vmiter(new_pagetable,stack_addr).try_map(phy_stack,PTE_P | PTE_W | PTE_U );
	if (check<0){
		arrfree(should_free,findex);
		return;
	}

    // mark process as runnable
    ptable[pid].state = P_RUNNABLE;
}



// exception(regs)
//    Exception handler (for interrupts, traps, and faults).
//
//    The register values from exception time are stored in `regs`.
//    The processor responds to an exception by saving application state on
//    the kernel's stack, then jumping to kernel assembly code (in
//    k-exception.S). That code saves more registers on the kernel's stack,
//    then calls exception().
//
//    Note that hardware interrupts are disabled when the kernel is running.

void exception(regstate* regs) {
    // Copy the saved registers into the `current` process descriptor.
    current->regs = *regs;
    regs = &current->regs;

    // It can be useful to log events using `log_printf`.
    // Events logged this way are stored in the host's `log.txt` file.
    /* log_printf("proc %d: exception %d at rip %p\n",
                current->pid, regs->reg_intno, regs->reg_rip); */

    // Show the current cursor location and memory state
    // (unless this is a kernel fault).
    console_show_cursor(cursorpos);
    if (regs->reg_intno != INT_PF || (regs->reg_errcode & PFERR_USER)) {
        memshow();
    }

    // If Control-C was typed, exit the virtual machine.
    check_keyboard();


    // Actually handle the exception.
    switch (regs->reg_intno) {

    case INT_IRQ + IRQ_TIMER:
        ++ticks;
        lapicstate::get().ack();
        schedule();
        break;                  /* will not be reached */

    case INT_PF: {
        // Analyze faulting address and access type.
        uintptr_t addr = rdcr2();
        const char* operation = regs->reg_errcode & PFERR_WRITE
                ? "write" : "read";
        const char* problem = regs->reg_errcode & PFERR_PRESENT
                ? "protection problem" : "missing page";

        if (!(regs->reg_errcode & PFERR_USER)) {
            panic("Kernel page fault for %p (%s %s, rip=%p)!\n",
                  addr, operation, problem, regs->reg_rip);
        }
        console_printf(CPOS(24, 0), 0x0C00,
                       "Process %d page fault for %p (%s %s, rip=%p)!\n",
                       current->pid, addr, operation, problem, regs->reg_rip);
        current->state = P_BROKEN;
        break;
    }

    default:
        panic("Unexpected exception %d!\n", regs->reg_intno);

    }


    // Return to the current process (or run something else).
    if (current->state == P_RUNNABLE) {
        run(current);
    } else {
        schedule();
    }
}


// syscall(regs)
//    System call handler.
//
//    The register values from system call time are stored in `regs`.
//    The return value, if any, is returned to the user process in `%rax`.
//
//    Note that hardware interrupts are disabled when the kernel is running.

int syscall_page_alloc(uintptr_t addr);

uintptr_t syscall(regstate* regs) {
    // Copy the saved registers into the `current` process descriptor.
    current->regs = *regs;
    regs = &current->regs;

    // It can be useful to log events using `log_printf`.
    // Events logged this way are stored in the host's `log.txt` file.
    /* log_printf("proc %d: syscall %d at rip %p\n",
                  current->pid, regs->reg_rax, regs->reg_rip); */

    // Show the current cursor location and memory state
    // (unless this is a kernel fault).
    console_show_cursor(cursorpos);
    memshow();

    // If Control-C was typed, exit the virtual machine.
    check_keyboard();


    // Actually handle the exception.
    switch (regs->reg_rax) {

    case SYSCALL_PANIC:
        panic(nullptr);         // does not return

    case SYSCALL_GETPID:
        return current->pid;

    case SYSCALL_YIELD:
        current->regs.reg_rax = 0;
        schedule();             // does not return

    case SYSCALL_PAGE_ALLOC:
        return syscall_page_alloc(current->regs.reg_rdi);

    case SYSCALL_FORK:
	return fork();

    case SYSCALL_EXIT:
	sys_exit();
	schedule();

    default:
        panic("Unexpected system call %ld!\n", regs->reg_rax);

    }

    panic("Should not get here!\n");
}


// syscall_page_alloc(addr)
//    Handles the SYSCALL_PAGE_ALLOC system call. This function
//    should implement the specification for `sys_page_alloc`
//    in `u-lib.hh` (but in the handout code, it does not).

int syscall_page_alloc(uintptr_t addr) {

	//Check the specifications:
	//check that it is a valid address:
	if (addr%4096 !=0 || addr<PROC_START_ADDR || addr>=MEMSIZE_VIRTUAL) return -1;
    //Allocate a new page and return if failed:
	void* paddr=kalloc(PAGESIZE);
	if (paddr==nullptr) {
		return -1;
	}
	//map the page:
    int check=vmiter(ptable[current->pid].pagetable,addr).try_map(paddr,PTE_P | PTE_W | PTE_U); // leak
	if (check<0){
        //clean up if try map failed:
		kfree(paddr);
		return -1;
	}
	return 0;
}


int fork(){

    //This is used to keep track of the should_free array:
	unsigned findex=0;

	//Look for a free slot in ptable:
	unsigned fslot;
	for (fslot = 1;fslot<NPROC;fslot++){
		if (ptable[fslot].state==P_FREE) break;
	}
	if (fslot==NPROC) return -1;

	//Allocate a new pagetable:
	x86_64_pagetable* child_pagetable=(x86_64_pagetable*)kalloc(PAGESIZE);
	if (child_pagetable==nullptr){
		return -1;
	}
	//Keep track:
	should_free[findex]=(uintptr_t)child_pagetable;
	findex++;

	//Copy to the new pagetable:
	vmiter childit(child_pagetable,0);
	vmiter parit(current->pagetable,0);
	while (parit.va()<MEMSIZE_VIRTUAL){
		if (!parit.present()) {}
		else if (parit.writable() && parit.user() && parit.pa()!=CONSOLE_ADDR ){
			//If the page is writable you have to use different physical address:
			void* newpage=kalloc(PAGESIZE);
			if (newpage==nullptr) {
				//Clean up if kalloc failed:
				arrfree(should_free,findex);
				return -1;
			}
			//Keep track:
			should_free[findex]=(uintptr_t)newpage;
			findex++;
			//Copy contents:
			memset(newpage,0,PAGESIZE);
			memcpy(newpage,(void*)parit.pa(),PAGESIZE);
			int check = childit.try_map(newpage,parit.perm());
			if (check<0) {
                //Clean up if failed:
				arrfree(should_free,findex);
				return -1;
			}
		}
		else{
            //Map to the same physical address:
			int check = childit.try_map(parit.pa(),parit.perm());
			if (check<0) {
				arrfree(should_free,findex);
				return -1;
			}
		}
		childit+=PAGESIZE;
		parit+=PAGESIZE;
    	}
	ptable[fslot].pagetable=child_pagetable;
	//Copy the regs except for rax:
	ptable[fslot].regs = current->regs;
	ptable[fslot].regs.reg_rax = 0;

	//Mark as runnable:
	ptable[fslot].state=P_RUNNABLE;
	return fslot;
}



// schedule
//    Pick the next process to run and then run it.
//    If there are no runnable processes, spins forever.

void schedule() {
    pid_t pid = current->pid;
    for (unsigned spins = 1; true; ++spins) {
        pid = (pid + 1) % NPROC;
        if (ptable[pid].state == P_RUNNABLE) {
            run(&ptable[pid]);
        }

        // If Control-C was typed, exit the virtual machine.
        check_keyboard();

        // If spinning forever, show the memviewer.
        if (spins % (1 << 12) == 0) {
            memshow();
            log_printf("%u\n", spins);
        }
    }
}


// run(p)
//    Run process `p`. This involves setting `current = p` and calling
//    `exception_return` to restore its page table and registers.

void run(proc* p) {
    assert(p->state == P_RUNNABLE);
    current = p;

    // Check the process's current pagetable.
    check_pagetable(p->pagetable);

    // This function is defined in k-exception.S. It restores the process's
    // registers then jumps back to user mode.
    exception_return(p);

    // should never get here
    while (true) {
    }
}


// memshow()
//    Draw a picture of memory (physical and virtual) on the CGA console.
//    Switches to a new process's virtual memory map every 0.25 sec.
//    Uses `console_memviewer()`, a function defined in `k-memviewer.cc`.

void memshow() {
    static unsigned last_ticks = 0;
    static int showing = 0;

    // switch to a new process every 0.25 sec
    if (last_ticks == 0 || ticks - last_ticks >= HZ / 2) {
        last_ticks = ticks;
        showing = (showing + 1) % NPROC;
    }

    proc* p = nullptr;
    for (int search = 0; !p && search < NPROC; ++search) {
        if (ptable[showing].state != P_FREE
            && ptable[showing].pagetable) {
            p = &ptable[showing];
        } else {
            showing = (showing + 1) % NPROC;
        }
    }

    extern void console_memviewer(proc* vmp);
    console_memviewer(p);
}


void sys_exit(){
    //Walk over the current page table:
	for (vmiter it(current->pagetable,0);
         it.va() < MEMSIZE_VIRTUAL;
         it += PAGESIZE) {

		if (!vmiter(kernel_pagetable,it.va()).present() && it.user() && it.pa()!=CONSOLE_ADDR && pages[(uintptr_t)it.pa()/PAGESIZE].refcount==1 )
        {
            //If it is user accessable, not a kernel page,not the console, and not shared then free it:
            kfree((void*)it.pa());
        }
		else if (!vmiter(kernel_pagetable,it.va()).present() && it.user() && it.pa()!=CONSOLE_ADDR && pages[(uintptr_t)it.pa()/PAGESIZE].refcount>1)
            //If it is a shared page then just mark that one of its proccesses has been freed:
			pages[(uintptr_t)it.pa()/PAGESIZE].refcount--;
    	}
    //Free the page tables:
	for (ptiter it(current->pagetable,0);
	it.active();
	it.next()){
			kfree((void*)it.pa());
	}
	//Mark the process as free
	ptable[current->pid].state=P_FREE;
	return;
}





