#define M61_DISABLE 1
#include "m61.hh"
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cinttypes>
#include <cassert>


#include <unordered_map>

#include <iostream>
using namespace std;

unsigned long long nactive=0;           // number of active allocations [#malloc - #free]
unsigned long long active_size=0;       // number of bytes in active allocations
unsigned long long ntotal=0;            // number of allocations, total
unsigned long long total_size=0;        // number of bytes in allocations, total
unsigned long long nfail=0;             // number of failed allocation attempts
unsigned long long fail_size=0;         // number of bytes in failed allocation attempts
uintptr_t heap_min;                   // smallest address in any region ever allocated
uintptr_t heap_max;                   // largest address in any region ever allocated
bool first_malloc=1;
unordered_map <void*,int> main_meta;                            //main metadata - stores data for free - stores the size or -1 if the pointer was freed
unordered_map <void*, pair<const char*,long>> meta_leak;       //second metadata - stores data for leak reports and error reports - stores the file and line for each pointer

unordered_map <long, pair<pair<unsigned long long,unsigned long long>,const char*>> meta_hh;
//third metadata - stores data for heavy hitter report
//for each line, it stores the size of the bytes allocated and the number of allocations for that line
//the number of allocations is used to detect the frequent hitters

unordered_map <void*, pair <void*,void*>> link_map;            //fourth metadata - used to detect double frees
void* last_ptr;                                                //used with the fourth metadata - it stores the last pointer that was added to the list

/// m61_malloc(sz, file, line)
///    Return a pointer to `sz` bytes of newly-allocated dynamic memory.
///    The memory is not initialized. If `sz == 0`, then m61_malloc must
///    return a unique, newly-allocated pointer value. The allocation
///    request was at location `file`:`line`.


void* m61_malloc(size_t sz, const char* file, long line) {
    (void) file, (void) line;   // avoid uninitialized variable warnings
    // Your code here.
	if (sz+16<sz) {
        //We will need 16 bytes to detect boundary write errors so we're checking that adding 16 to the size won't cause overflow
		nfail++;
        fail_size+=sz;
		return nullptr;
	}
    void* p=base_malloc(sz+16);
    if (p!=nullptr){
        nactive++;
        active_size+=sz;

        main_meta[p]=sz;

        meta_leak[p].first=file;
        meta_leak[p].second=line;

        meta_hh[line].first.first+=sz;
        meta_hh[line].first.second+=1;
        meta_hh[line].second=file;

        ntotal++;
        total_size+=sz;

        unsigned long long int h=-1; //magic number to check boundary write errors
        memcpy(p+sz,&h,8);
        memcpy(p+sz+8,&h,8);

        if (first_malloc){
            //store the initial values:
            link_map[p].first=nullptr;
            link_map[p].second=nullptr;
            last_ptr=p;
            first_malloc=0;
            heap_min=(uintptr_t)p;
            heap_max=(uintptr_t)p +sz+16;
        }
        else{
            //compare and then store:
            link_map[last_ptr].second=p;
            link_map[p].first=last_ptr;
            link_map[p].second=nullptr;
            last_ptr=p;
            heap_min=min(heap_min,(uintptr_t)p);
            heap_max=max(heap_max,(uintptr_t)p+sz+16);
        }
        return p;
    }
    else {
        nfail++;
        fail_size+=sz;
        return p;
    }
}


/// m61_free(ptr, file, line)
///    Free the memory space pointed to by `ptr`, which must have been
///    returned by a previous call to m61_malloc. If `ptr == NULL`,
///    does nothing. The free was called at location `file`:`line`.

void m61_free(void* ptr, const char* file, long line) {
    (void) file, (void) line;   // avoid uninitialized variable warnings
    // Your code here.

	if (ptr==nullptr) return;

	//check if the pointer has never been allocated:
    if (main_meta[ptr]==0) {
        //check if it is inside the heap:
        if ((uintptr_t)ptr>=heap_min && (uintptr_t)ptr<=heap_max){
            cerr<<"MEMORY BUG: "<<file<<":"<<line<<": invalid free of pointer "<<ptr<<", not allocated"<<endl;
            //check if it is inside another allocation:
            for (auto it:main_meta){
                if (ptr>it.first && ptr<=it.first+it.second){  //ptr is bigger than a previous allocated pointer and smaller than (allocated pointer + size)
                    cerr<<"  "<<meta_leak[it.first].first<<":"<<meta_leak[it.first].second<<": "<<ptr<<" is "<<(uint64_t)ptr-(uint64_t)it.first<<" bytes inside a "<<it.second<<" byte region allocated here"<<endl;
                    break;
                }
            }
            abort();
        }
        //the pointer is outside the heap:
        cerr<<"MEMORY BUG: "<<file<<":"<<line<<": invalid free of pointer "<<ptr<<", not in heap"<<endl;
        abort();
    }

    //Check for double free:
	bool double_freed=0;
        //Store the next and previous pointers:
	void* previous=link_map[ptr].first;
	void* next=link_map[ptr].second;

        //Check if the previous pointer points to current pointer:
	if (previous!=nullptr && link_map[previous].second!=ptr)
        double_freed=1;

        //Check if the next pointer points back to current pointer:
	if (next!=nullptr && link_map[next].first!=ptr)
        double_freed=1;

        //Check if all the pointers have been freed:
	if (last_ptr==nullptr)
        double_freed=1;

	if (double_freed==1){
		cerr<<"MEMORY BUG: "<<file<<":"<<line<<": invalid free of pointer "<<ptr<<", double free"<<endl;
        abort();
	}



    //Check for out of boundary writing:
    unsigned long long int h=-1; //the magical number
	if ((memcmp(ptr+main_meta[ptr],&h,8)!=0) || (memcmp(ptr+main_meta[ptr]+8,&h,8)!=0))
	{
		cerr<<"MEMORY BUG: "<<file<<":"<<line<<": detected wild write during free of pointer "<<ptr<<endl;
		abort();
	}



    //All checks are passed - this is a proper free:

    //Modify the linked list:
	if (previous!=nullptr) link_map[previous].second=next;
	if (next!=nullptr) link_map[next].first=previous;
	if (ptr==last_ptr) {
		last_ptr=previous;
	}

    nactive--;
    active_size-=main_meta[ptr];
    main_meta[ptr]=-1; //Storing -1 to say that it has been freed (for leak checks)
    base_free(ptr);
}


/// m61_calloc(nmemb, sz, file, line)
///    Return a pointer to newly-allocated dynamic memory big enough to
///    hold an array of `nmemb` elements of `sz` bytes each. If `sz == 0`,
///    then must return a unique, newly-allocated pointer value. Returned
///    memory should be initialized to zero. The allocation request was at
///    location `file`:`line`.

void* m61_calloc(size_t nmemb, size_t sz, const char* file, long line) {
    // Your code here (to fix test014).
	void* ptr;
	//Check if nmemb * sz <= Unsigned long long max , we can do this without overflowing by moving sz to the other side of the inequality:
	if (nmemb<=(unsigned long long)18446744073709551615/sz){
        //We can send this value to malloc:
        ptr = m61_malloc(nmemb * sz, file, line);
	}
	else{
	    //This is a very big size and we can't allocate it:
		ptr=nullptr;
		nfail++;
        fail_size+=sz*nmemb;
	}
	if (ptr) {
        	memset(ptr, 0, nmemb * sz);
    }
    return ptr;
}


/// m61_get_statistics(stats)
///    Store the current memory statistics in `*stats`.

void m61_get_statistics(m61_statistics* stats) {
    // Stub: set all statistics to enormous numbers
    memset(stats, 255, sizeof(m61_statistics));
    // Your code here.
    //Move the values from the global variables to the struct
    stats->nactive=		nactive;
    stats->active_size=	active_size;       // number of bytes in active allocations
    stats->ntotal=		ntotal;            // number of allocations, total
    stats->total_size=	total_size;        // number of bytes in allocations, total
    stats->nfail=		nfail;             // number of failed allocation attempts
    stats->fail_size=	fail_size;         // number of bytes in failed allocation attempts
    stats->heap_min=	heap_min;          // smallest address in any region ever allocated
    stats->heap_max=	heap_max;          // largest address in any region ever allocated
    //*stats.nactive=nactive;
    //*stats.active_size=active_size;

}


/// m61_print_statistics()
///    Print the current memory statistics.

void m61_print_statistics() {
    m61_statistics stats;
    m61_get_statistics(&stats);
    printf("alloc count: active %10llu   total %10llu   fail %10llu\n",
           stats.nactive, stats.ntotal, stats.nfail);
    printf("alloc size:  active %10llu   total %10llu   fail %10llu\n",
           stats.active_size, stats.total_size, stats.fail_size);
}


/// m61_print_leak_report()
///    Print a report of all currently-active allocated blocks of dynamic
///    memory.

void m61_print_leak_report() {
    // Your code here.
    //Leak Check:
	for (auto it:main_meta){
        //Check if the pointer was freed:
		if (it.second==-1)continue;
        //report a leak:
		cout<<"LEAK CHECK: "<<meta_leak[it.first].first<<":"<<meta_leak[it.first].second<<": allocated object "<<it.first<<" with size "<<it.second<<endl;
	}
	//LEAK CHECK: test033.cc:23: allocated object 0x9b811e0 with size 19
}


/// m61_print_heavy_hitter_report()
///    Print a report of heavily-used allocation locations.

void m61_print_heavy_hitter_report() {
    // Your heavy-hitters code here
	for (auto it:meta_hh){

        //Check for Memory heavy hitter:
		if (10*it.second.first.first>2*total_size){
			cout<<"HEAVY HITTER: "<<it.second.second<<":"<<it.first<<": "<<it.second.first.first<<" bytes (~"<<(double)it.second.first.first/total_size*100.0<<"%)"<<endl;
		}

        //Check for frequent heavy hitter:
		if (10*it.second.first.second>2*ntotal){
			cout<<"HEAVY HITTER: "<<it.second.second<<":"<<it.first<<": "<<it.second.first.second<<" allocations (~"<<(double)it.second.first.second/ntotal*100.0<<"%)"<<endl;
		}
	}
}
