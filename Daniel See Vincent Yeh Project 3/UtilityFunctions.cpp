#include "VirtualMachine.h"
#include "Machine.h"
#include "UtilityFunctions.h"
#include <stdint.h>
#include <stdio.h>

int w = 1;

//=== Mutex =======================================================

Mutex::Mutex(){
	ThreadStore *tStore = ThreadStore::getInstance();
	id									= 	tStore->getNumMutexes() + 1;	//never decreases, here or in ThreadStore
	owner			 					= NULL;	//also used as the lock state
	priorityLists[0x00] = new std::list<TCB*>();
	priorityLists[0x01] = new std::list<TCB*>();
	priorityLists[0x02] = new std::list<TCB*>();
}

Mutex::~Mutex(){

	for(int i = 0x0; i < 0x03; i++){
		while(priorityLists[i]->empty() == false){	//while the list isn't empty
			priorityLists[i]->pop_front();						//take stuff out of it
		}
		delete priorityLists[i];										//once it's empty, delete it
	}
}

void Mutex::release(){

	ThreadStore *tStore = ThreadStore::getInstance();
	TVMThreadPriority oldPriority = owner->getPriority();
	owner->removeMutex(this);	//the old owner is done, so isn't waiting on this mutex anymore

	if((priorityLists[0x00]->empty() == true) && (priorityLists[0x01]->empty() == true) && (priorityLists[0x02]->empty() == true)){
		//all prios empty, mutex has no owner and schedules no new threads
		owner = NULL;
	}
	else{																								//at least one prio is waiting
		for(TVMThreadPriority i = 0x02; i >= 0x00; i--){	//check prios starting high
			if(priorityLists[i]->empty() == false){					//if another thread waiting on mutex
				owner = priorityLists[i]->front();						//set new mutex owner to be that thread
				priorityLists[i]->pop_front();								//pop	front, as thread no longer waiting
				break;																				//stop looking for new thread
			}
		}
		TVMThreadState savedState = owner->getState();
		owner->sleep(0);	//set ticks to sleep = 0
		owner->setState(savedState);
		//release of mutex causes higher-prio thread to be scheduled
		if(owner->getPriority() > oldPriority){	//if new thread, eg thread that was waiting, more important than old
			tStore->scheduleThreadEarly(owner);		//remove from waitlist early and run scheduler
		//	tStore->removeFromWaitlistEarly(owner);
		//	schedule(VM_THREAD_PRIORITY_HIGH);
		}
		else
			tStore->removeFromWaitlistEarly(owner);
	}
}

void Mutex::lock(TCB *thread, TVMTick timeout){	//called from VMMutexAcquire
	ThreadStore *tStore = ThreadStore::getInstance();
	TCB* currentThread = tStore->getCurrentThread();

	if(owner == NULL){	//if not owned
		owner = tStore->getCurrentThread();	//give to asking thread
		currentThread->addMutex(this, false);			//add mutex, not waiting on it
		return;
	}
	
	if((timeout == VM_TIMEOUT_IMMEDIATE) && (owner != NULL)){	
		return;	//return immediately if mutex is locked, don't add to mutexes list
	}
	else{
		thread->setWaitingOnMutex(true);	//set thread waiting on mutex
		if(timeout == VM_TIMEOUT_INFINITE){
			tStore->getCurrentThread()->addMutex(this, true);		//add mutex, waiting
			priorityLists[currentThread->getPriority() - 1]->push_back(currentThread);	//add thread to mutex wait list
			tStore->sleepCurrentThread(-1);	//insert thread into waitlist with ticksToSleep = timeout
		}
		else{
			tStore->getCurrentThread()->addMutex(this, true);	//add mutex, waiting
			priorityLists[currentThread->getPriority() - 1]->push_back(currentThread);	//add thread to mutex wait list
			tStore->sleepCurrentThread(timeout);
		}
	}
}

void Mutex::deleteFromVM(){
	for(int i = 0x0; i < 0x03; i++){	//a thread cannot wait on a deleted mutex
		while(priorityLists[i]->empty() == false){	//while the list isn't empty
			priorityLists[i]->front()->removeMutex(this);					//indicate threads are no longer waiting on this mutex
			priorityLists[i]->pop_front();						//take stuff out of it
		}
	}
	owner 	= NULL;	//a deleted mutex cannot be held
	deleted = true;
}

bool Mutex::isDeleted(){
	return deleted;
}

TCB* Mutex::getOwner(){	//a free mutex has a null owner
	return owner;
}

TVMMutexID Mutex::getID(){
	return id;
}

//=================================================================


//=== TCB =======================================================

extern "C" {
	TCB::~TCB(){

		if(entryFunction == (TVMThreadEntry)&VMStart){
			delete (uint8_t*)stackBaseAddress;
		}
		else{
			ThreadStore* tStore = ThreadStore::getInstance();
			MemoryPool* systemMemoryPool = tStore->findMemoryPoolByID(0);
			systemMemoryPool->deallocate((uint8_t*)stackBaseAddress);
		}

//		delete (uint8_t*)(stackBaseAddress);	//free memory called by new
		safeEntryParam[0] = NULL;
		safeEntryParam[1] = NULL;
		delete safeEntryParam;
		delete contextRef;
	}
	
	TCB::TCB(TVMThreadEntry entry, void *param, TVMMemorySize memsize, TVMThreadPriority prio, TVMThreadIDRef tid){
		//Initialize all variables in thread control block
		//These initializations are common to all threads:

		//assign IDs in ascending order, return the ID to the calling function by reference
		ThreadStore* tStore = ThreadStore::getInstance();
		threadID = tStore->getNumThreads() + 1;
//		threadID = ThreadStore::getInstance()->getNumThreads() + 1;
		*tid = threadID;

		TVMThreadEntry safeEntryFunction 	= &safeEntry;				//this is the skeleton function
		safeEntryParam 										= new int*[2];
		safeEntryParam[0]									= (int *)entry;			//skeleton function's params - [0] is thread entry func, [1] is param for thread entry func
		safeEntryParam[1]									= (int *)param;

		waitingMemoryResult = NULL;	
		mutexes 					= new std::list<MutexInfo*>();		//the mutex the thread is waiting on. Threads dont start with a mutex.
		deleted		 				= false;
		priority 				 	= prio;
		entryFunction 		= entry;
		entryParam 				= param;			//cast param to int*, assign
		stackSize 			 	= memsize;
		fileIOResult 			= 0;
		ticksToSleep		 	= 0;
		contextRef 				= new SMachineContext[1];
		waitingOnIO 			= false;
		
		if(entry == (TVMThreadEntry)&VMStart){				//indicates creation of vmStart thread, which already has a context 
			//vmStart is running
			state 					 = VM_THREAD_STATE_RUNNING;			//vmStart is obviously running
			stackBaseAddress = new uint8_t[1];							//may need to change this later
		}
		else if(entry == (TVMThreadEntry)&idle){					//initializing idle thread
			state = VM_THREAD_STATE_READY;
			MemoryPool* systemMemoryPool = tStore->findMemoryPoolByID(0);	
			stackBaseAddress = systemMemoryPool->allocateMemory(memsize);

			if(stackBaseAddress == NULL){
				stackBaseAddress = tStore->waitCurrentThreadOnMemory(memsize, 0);
			}
		
//			stackBaseAddress 	= new uint8_t[memsize];				//allocate stack
			//assign context to new machine context, e.g. context = Machine->newContext(params...);
			MachineContextCreate(contextRef, &idle, NULL, stackBaseAddress, stackSize);
		}
		else{	
			state 					 	= VM_THREAD_STATE_DEAD;				//All other threads begin in dead state
			MemoryPool* systemMemoryPool = tStore->findMemoryPoolByID(0);
			stackBaseAddress = systemMemoryPool->allocateMemory(memsize);
			
			if(stackBaseAddress == NULL){
				stackBaseAddress = tStore->waitCurrentThreadOnMemory(memsize, 0);
			}
//			stackBaseAddress 	= new uint8_t[memsize];				//allocate stack
			//assign context to new machine context, e.g. context = Machine->newContext(params...);
			MachineContextCreate(contextRef, safeEntryFunction, safeEntryParam, stackBaseAddress, stackSize);
		}
	}
}

void TCB::setIsDeleted(bool d){
	deleted = d;
}

bool TCB::isDeleted(){
	return deleted;
}

void TCB::setFileIOResult(int result){
	fileIOResult = result;
}

int TCB::getFileIOResult(){
	return fileIOResult;
}

void TCB::addMutex(Mutex *m, bool w){
	MutexInfo *mInfo = new MutexInfo();
	mInfo->mutex = m;
	mInfo->waiting = w;
	mutexes->push_back(mInfo);
}

void TCB::removeMutex(Mutex *m){

	for(std::list<MutexInfo*>::iterator i = mutexes->begin(); i != mutexes->end(); i++){	//iterate over entire wait list
		if((*i)->mutex == m){
			mutexes->erase(i);																			//remove thread from wait list
			i--;																										//decrement iterator
			break;
		}
	}
}

Mutex* TCB::getWaitingMutex(){
	if(mutexes->empty() == false){
		return mutexes->back()->mutex;
	}
	else
		return NULL;
}

bool TCB::isWaitingOnMutex(){
	if(mutexes->empty() == false){
		return mutexes->back()->waiting;
	}
	else{
		return false;
	}
}

void TCB::setWaitingOnMutex(bool w){
	if(mutexes->empty() == false){
		mutexes->back()->waiting = w;
	}
}

void TCB::setIsWaitingOnIO(bool waiting){
	waitingOnIO = waiting;
}

bool TCB::isWaitingOnIO(){
	return waitingOnIO;
}

void TCB::releaseMutexes(){
	while(mutexes->empty() == false){
		MutexInfo *m = mutexes->front();
		mutexes->pop_front();
		m->mutex->release();
		delete m;
	}
}

TVMThreadID TCB::getThreadID(){
	return threadID;
}

TVMThreadPriority TCB::getPriority(){
	return priority;
}

TVMThreadState TCB::getState(){
	return state;
}

TVMThreadEntry TCB::getEntryFunction(){
	return entryFunction;
}

void TCB::setState(TVMThreadState s){
	state = s;
}

void TCB::sleep(TVMTick tick){
	state = VM_THREAD_STATE_WAITING;	//puts the thread into the waiting state for tick ticks
	ticksToSleep = tick;	//sets how long the thread should sleep
}

TVMTick TCB::getTicksToSleep(){
	return ticksToSleep;
}

void TCB::decrementTicksToSleep(){
	ticksToSleep = ticksToSleep - 1;
}

void TCB::setWaitingMemoryResult(uint8_t* r){
	waitingMemoryResult = r;
}

uint8_t* TCB::getWaitingMemoryResult(){
	return waitingMemoryResult;
}

//=================================================================


//=== MemoryPool ==================================================

MemoryPool::MemoryPool(uint8_t *addr, TVMMemorySize size){

		id = ThreadStore::getInstance()->getNumMemoryPools() + 1;
		allocatedChunkVector = new std::vector<MemoryChunk*>();
		baseAddress = addr;
		poolSize = size;
}

TVMMemorySize MemoryPool::getSize(){
	return poolSize;
}

void MemoryPool::addChunk(MemoryChunk* chunk){
	//inserts a new chunk into the list of tracked chunks, ensuring that the vector remains sorted
	//inserting into a vector always puts the new item in the space BEFORE the iterator!
	std::vector<MemoryChunk*>::iterator itr = allocatedChunkVector->begin();
	
	//case 1: There is nothing in the list. In this case, just add to the end
	if(allocatedChunkVector->empty() == true){
		allocatedChunkVector->push_back(chunk);
	}
	else if(chunk->startAddress > allocatedChunkVector->back()->startAddress){
		//case 2: the new chunk's address is larger than the last address. In this case, also add to the end of the vector
		allocatedChunkVector->push_back(chunk);
	}
	else{
		//case 3: the new chunk must be inserted somewhere in the middle of the list, so find that place and put it there
		for(int i = 0; i < allocatedChunkVector->size(); i++){
			if(chunk->startAddress < (*allocatedChunkVector)[i]->startAddress){ //if the new chunk to be inserted begins BEFORE the current chunk,
				allocatedChunkVector->insert(itr, chunk);	//insert the new chunk into the place before the current chunk
				break;
			}
			itr++;
		}
	}
}

bool MemoryPool::deallocate(uint8_t* chunk){

	TVMMemorySize foundSize = 0;
	ThreadStore *tStore = ThreadStore::getInstance();
	bool foundFlag = false;

//	printf("MemoryPool::deallocate(): looking for chunk %d.\n", chunk);
	for(unsigned int i = 0; i < allocatedChunkVector->size(); i++){
		if(chunk == (*allocatedChunkVector)[i]->startAddress){ //if the new chunk to be inserted begins BEFORE the current chunk,
//			printf("\nMemoryPool::deallocate() - deallocated chunk %d\n", (*allocatedChunkVector)[i]->startAddress);
			foundFlag = true;
			foundSize = (*allocatedChunkVector)[i]->length;
			allocatedChunkVector->erase(allocatedChunkVector->begin() + i);
			break;
		}
	}

	if((foundFlag == true) && ((id == 1) || (id == 0))){
//		printf("\nMemoryPool::deallocate() - mid %d calling signalMemoryRelease with size %d\n", id, foundSize);
		tStore->signalMemoryRelease(foundSize, id);
		return true;
	}
	else if(foundFlag == true){
		return true;
	}
	else{
//		printf("\nMemoryPool::deallocate() - MID %d failed to deallocated chunk %p.\n", id, chunk);
		return false;	//returns false if the pool does not contain the chunk to be deallocated
	}
}

uint8_t* MemoryPool::allocateMemory(TVMMemorySize size){

	size = (size + 0x3F)&(~0x3F);	//first, round the chunk up to the nearest 64 bytes	
	uint8_t* nextFreeSpace = getNextSpace(size);
		
	if(nextFreeSpace != NULL){
//		printf("MemoryPool::allocateMemory(): allocated address %d\n", nextFreeSpace);
//		printf("MemoryPool::allocateMemory(): MID %d about to add chunk of size %d with start address %d.\n", id, size, nextFreeSpace);
		MemoryChunk *chunk = new MemoryChunk;
		chunk->startAddress = nextFreeSpace;
		chunk->length = size;
		
		addChunk(chunk);
	}
//	else
//		printf("\nMemoryPool::allocate() - failed to find free space.\n");
	
	return nextFreeSpace;
}

uint8_t* MemoryPool::getNextSpace(TVMMemorySize size){

//	printf("MemoryPool::getNextSpace(): MID %d looking for space\n", id);
	//Case 1: there are no allocated objects. Solution: return the base address as the first eligible space.
	if(allocatedChunkVector->empty() == true){
		if(poolSize >= size){		//if the pool is big enough to hold the object
//			printf("MemoryPool::getNextSpace(): MID %d allocated chunk vector is empty, returning base address %d\n", id, baseAddress);
			return baseAddress;		//allocate the memory
		}
		else{
			printf("MemoryPool::getNextSpace(): MID %d allocated chunk vector is empty, but poolsize is smaller than size to allocate!\n", id);
			return NULL;
		}
	}
//	else
//		printf("MemoryPool::getNextSpace(): MID %d allocated chunk vector not empty!\n", id);

	//Case 2: There is at least one allocated object. Solution: Check if there is space ahead of it. If there is, allocate that.
	if(allocatedChunkVector->size() >= 1){
		
		if((allocatedChunkVector->front()->startAddress - baseAddress) >= size){	//if there is enough room in front of the first object,
//			printf("MemoryPool::getNextSpace(): MID %d found room in front of first object, allocating base address %d\n", id, baseAddress);
			return baseAddress;																						//allocate the space in front
		}

		//Case 3: There is no room in front of the first object. Solution: try to allocate the space between objects.
		for(int i = 0; i < allocatedChunkVector->size() - 1; i++){

			//If the space difference between beginning of the next object and the end of the current object, is big enough to hold the new object
			if((*allocatedChunkVector)[i + 1]->startAddress - ((*allocatedChunkVector)[i]->startAddress + (*allocatedChunkVector)[i]->length) >= size){
//				printf("MemoryPool::getNextSpace(): MID %d allocated space between two objects\n", id);
				return (*allocatedChunkVector)[i]->startAddress + (*allocatedChunkVector)[i]->length;	
				//allocate the space at the end of the current object
			}
		}
			
		//Case 4: There is no room in front of the list or in between objects. Solution: try to allocate behind the last object in the list.
		if((baseAddress + poolSize) - (allocatedChunkVector->back()->startAddress + allocatedChunkVector->back()->length) >= size){
			//If the space between the end of the pool and the end of the last object is greater than the size of the object we're trying to add,
//			printf("MemoryPool::getNextSpace(): MID %d allocated chunk vector behind the last object\n", id);
//			printf("MemoryPool::getNextSpace(): the length of the last object is %d\n", allocatedChunkVector->back()->length);
			return (allocatedChunkVector->back()->startAddress + allocatedChunkVector->back()->length);
			//put the new object behind the last object
		}
	}

//	printf("MemoryPool::getNextSpace(): MID %d could not find any room to allocate a new chunk of size %d\n", id, size);
	return NULL;	//if control gets here, nothing above worked, there is no free space
}

bool MemoryPool::isAddressInRange(uint8_t* address){

	if((address > (baseAddress + poolSize)) || (address < baseAddress)){
		return false;
	}
	else{
		return true;
	}
}

TVMMemorySize MemoryPool::getNumberOfUnallocatedBytes(){
	//calculates the remaining size of the pool by subtracting the size of each existing
	//chunk from the pool's original size

	TVMMemorySize unallocated = poolSize;

	for(int i = 0; i < allocatedChunkVector->size(); i++){
		unallocated = unallocated - (*allocatedChunkVector)[i]->length;
	}
	return unallocated;
}

bool MemoryPool::isInUse(){
	
//	printf("MemoryPool::isInUse(): MID %d vector is empty: %d\n", id, allocatedChunkVector->empty());

//Debug, should not need:	
	if(allocatedChunkVector->empty() == false){
		printf("MemoryPool::isInUse(): MID %d vector has %d allocations: \n", id, allocatedChunkVector->size());
		for(int i = 0; i < allocatedChunkVector->size(); i++){
			printf("MemoryPool::isInUse(): MID %d vector allocation: startAddress %p, length %d\n", id, (*allocatedChunkVector)[i]->startAddress,
				(*allocatedChunkVector)[i]->length);
		}
	}

	return allocatedChunkVector->empty() == false;	//returns the opposite of allocatedChunkVector->empty(),
	//so returns true if the allocated chunk vector is NOT empty. If the pool is in use, then the chunk 
	//vector must not be empty, as allocations must exist. And if no allocations exist, then the vector 
	//must be empty, which means the pool is not in use
}

TVMMemoryPoolID MemoryPool::getID(){
	return id;
}

MemoryPool::~MemoryPool(){
	baseAddress = NULL;		//since the pool's base address is provided externally,
	//the pool does not "own" its own memory. Therefore, it should not be
	//responsible for deleting it, since other resources might depend on that
	//memory still existing. However, the pool can set its reference to that
	//memory to NULL.
	//For pools that we create ourselves (eg, the thread-wide pool), we can
	//delete that memory in the ThreadStore destructor.
}
//=================================================================



//=== ThreadStore ==================================================

ThreadStore *ThreadStore::DUniqueInstance = NULL;

void ThreadStore::sleepCurrentThread(TVMTick tick){

	currentThread->sleep(tick);	//set current thread's ticksToSleep to tick, set state to WAITING

	if(tick == 0)															//if sleep was called with timeout immediate
		schedule(currentThread->getPriority());	//attempt to schedule new thread of current priority
	else
		schedule(VM_THREAD_PRIORITY_HIGH);			//attempt to schedule high priority thread
}

Mutex* ThreadStore::findMutexByID(TVMMutexID mutexID){
	if((mutexID >= mutexVector->size()) || (mutexID < 0)){
		return NULL;							//mutexID is out of bounds
	}
	return (*mutexVector)[mutexID];	//otherwise return the mutex
}

void ThreadStore::deleteMemoryPool(TVMMemoryPoolID memoryPoolID){

	if((memoryPoolID >= memoryPoolVector->size()) || (memoryPoolID < 0)){	//if the memory pool ID is in range
		MemoryPool* poolToDelete = (*memoryPoolVector)[memoryPoolID];				//find the pool to delete
		(*memoryPoolVector)[memoryPoolID] = NULL;	//set its location to NULL to preserve the orderedness of the vector
		if(poolToDelete != NULL){									//if the pool to delete exists
			delete poolToDelete;										//delete it
		}
	}
}

MemoryPool* ThreadStore::findMemoryPoolByID(TVMMemoryPoolID memoryPoolID){
	if((memoryPoolID >= memoryPoolVector->size()) || (memoryPoolID < 0)){
		//printf("findMemoryPoolByID(): PoolID %d, memoryPoolVectorSize: %d\n", memoryPoolID, memoryPoolVector->size());
		return NULL;							//the memory ID is out of bounds
	}
//	printf("findMemoryPoolByID(): PoolID %d, memoryPoolVectorSize: %d\n", memoryPoolID, memoryPoolVector->size());
//	printf("findMemoryPoolByID(): %d\n", (* memoryPoolVector)[memoryPoolID]);
	return (*memoryPoolVector)[memoryPoolID];	//otherwise return the memory pool
}

TVMMutexID ThreadStore::getNumMutexes(){
	return numMutexes;
}

void ThreadStore::removeFromWaitlistEarly(TCB* thread){
	
	for(std::list<TCB*>::iterator i = waitList->begin(); i != waitList->end(); i++){	//iterate over entire wait list
		if(((*i) == thread) && ((*i)->isWaitingOnIO() == false)){	//if thread found and thread not waiting on IO
			thread->sleep(0);																				//set ticks to sleep zero
			thread->setState(VM_THREAD_STATE_READY);								//set state ready
			readyLists[thread->getPriority()]->push_back(thread);		//put thread in ready list
			waitList->erase(i);																			//remove thread from wait list
			i--;																										//decrement iterator
			break;																									//stop looking
		}
	}
}

void ThreadStore::runThreadEarly(TCB* thread){
	//TCB* foundThread = NULL;

	bool foundFlag = false;	

	for(std::list<TCB*>::iterator i = waitList->begin(); i != waitList->end(); i++){	//iterate over entire wait list
		if(((*i) == thread) && ((*i)->isWaitingOnIO() == false)){	//if thread found and thread not waiting on IO
			//foundThread = (*i);
			foundFlag = true;
			thread->sleep(0);																				//set ticks to sleep zero
			thread->setState(VM_THREAD_STATE_READY);								//set state ready
			readyLists[thread->getPriority()]->push_front(thread);	//put thread in FRONT of ready list
			waitList->erase(i);																			//remove thread from wait list
			i--;																										//decrement iterator
			break;																									//stop looking
		}
	}
//	if(foundThread == thread){					//if a thread was found
	if(foundFlag == true){
		//printf("runThreadEarly(): about to run TID %d early\n", thread->getThreadID());
		schedule(thread->getPriority());	//run scheduler, is outside FOR because schedule does not return
	}
}

void ThreadStore::scheduleThreadEarly(TCB* thread){
	//finds given thread in waitlist, takes it out of waitlist, runs scheduler
	TCB* foundThread = NULL;
	
	for(std::list<TCB*>::iterator i = waitList->begin(); i != waitList->end(); i++){	//iterate over entire wait list
		if(((*i) == thread) && ((*i)->isWaitingOnIO() == false)){	//if thread found and thread not waiting on IO
			foundThread = (*i);
			thread->sleep(0);																				//set ticks to sleep zero
			thread->setState(VM_THREAD_STATE_READY);								//set state ready
			readyLists[thread->getPriority()]->push_back(thread);		//put thread in ready list
			waitList->erase(i);																			//remove thread from wait list
			i--;																										//decrement iterator
			break;																									//stop looking
		}
	}
	if(foundThread == thread){	//if a thread was found
		schedule(VM_THREAD_PRIORITY_HIGH);	//run scheduler, is outside FOR because schedule does not return
	}
}

void ThreadStore::signalMemoryRelease(TVMMemorySize size, TVMMemoryPoolID mid){

	TCB* waitingThread = NULL;

	if((memoryWaitingLists[0]->empty() == true) && (memoryWaitingLists[1]->empty() == true) && (memoryWaitingLists[2]->empty() == true)){
		//all prios empty, mutex has no owner and schedules no new threads
//		printf("signalMemoryRelease(): all memory waiting lists are empty\n");
		return;
	}
	else{																									//at least one prio is waiting on memory
		for(int i = 2; i >= 0; i--){		//check prios starting high
			if(memoryWaitingLists[i]->empty() == false){			//if found waiting thread
				for(std::list<MemoryWaitingInfo*>::iterator itr = memoryWaitingLists[i]->begin(); itr != memoryWaitingLists[i]->end(); itr++){
					if(((*itr)->neededSize == size) && ((*itr)->pool == mid)){	//if found entry with needed size that equals the size that was just released
						waitingThread = (*itr)->thread;
						memoryWaitingLists[i]->erase(itr);
						break;
					}
				}
			}
		}
	}
	if(waitingThread != NULL){	//if a thread waiting on a chunk of size "size" was found
		waitingThread->setIsWaitingOnIO(false);
		waitingThread->setWaitingMemoryResult(findMemoryPoolByID(mid)->allocateMemory(size));

//		printf("signalMemoryRelease(): found thread %d, waiting result is %p", waitingThread->getThreadID(), waitingThread->getWaitingMemoryResult());
	
		if(currentThread->getPriority() < waitingThread->getPriority()){	//if new thread, eg thread that was waiting, more important than old
				scheduleThreadEarly(waitingThread);		//remove from waitlist early and run scheduler
			//	tStore->removeFromWaitlistEarly(owner);
			//	schedule(VM_THREAD_PRIORITY_HIGH);
			}
			else
				removeFromWaitlistEarly(waitingThread);
//		runThreadEarly(waitingThread);		//run that thread immediately. Change to either "schedule early" or "schedule"
	}
}

uint8_t* ThreadStore::waitCurrentThreadOnMemory(TVMMemorySize size, TVMMemoryPoolID mid){

//	printf("waitCurrentThreadOnMemory(): TID %d waiting %d byes from MID %d\n", currentThread->getThreadID(), size, mid);

	currentThread->setIsWaitingOnIO(true);
	currentThread->setState(VM_THREAD_STATE_WAITING);
	MemoryWaitingInfo* info = new MemoryWaitingInfo;
	info->thread = currentThread;
	info->neededSize = size;
	info->pool = mid;
	memoryWaitingLists[currentThread->getPriority() - 1]->push_back(info);
	schedule(VM_THREAD_PRIORITY_HIGH);	//this call should not return until sufficient memory is available
	return currentThread->getWaitingMemoryResult();
//	return findMemoryPoolByID((TVMMemoryPoolID)mid)->allocateMemory(size);	
}

void ThreadStore::waitCurrentThreadOnIO(){

	currentThread->setIsWaitingOnIO(true);
	currentThread->setState(VM_THREAD_STATE_WAITING);
	schedule(VM_THREAD_PRIORITY_HIGH);
	return;
}

void ThreadStore::deleteDeadThread(TCB* thread){

	for(std::list<TCB*>::iterator TCBItr = deadList->begin(); TCBItr != deadList->end(); TCBItr++){		//check deadList
		if((*TCBItr)->getThreadID() == thread->getThreadID()){	//if found id matches requested id
			//deadList->erase(TCBItr);
			thread->setIsDeleted(true);
			//delete thread;
			break;
		}
	}
	schedule(VM_THREAD_PRIORITY_HIGH);
}

TCB* ThreadStore::findThreadByID(TVMThreadID id){
	//searches every container for a thread matching id

	if(currentThread->getThreadID() == id){
		return currentThread;
	}
	for(TVMThreadPriority priority = 0x01; priority < 0x04; priority++){	//check all ready lists except idle list
		for(std::list<TCB*>::iterator TCBItr = readyLists[priority]->begin(); TCBItr != readyLists[priority]->end(); TCBItr++){
			//check every item in readyList[priority]
			if((*TCBItr)->getThreadID() == id){	//if found id matches requested id
				return *TCBItr;	//return TCB*
			}
		}
	}
	
	for(std::list<TCB*>::iterator TCBItr = waitList->begin(); TCBItr != waitList->end(); TCBItr++){ //check wait list
		if((*TCBItr)->getThreadID() == id){	//if found id matches requested id
			return *TCBItr;	//return TCB*
		}
	}
	
	for(std::list<TCB*>::iterator TCBItr = deadList->begin(); TCBItr != deadList->end(); TCBItr++){			//check deadList
		if(((*TCBItr)->getThreadID() == id) && ((*TCBItr)->isDeleted() == false)){	//if found id matches requested id and thread not deleted
			return *TCBItr;	//return TCB*
		}
	}
	return NULL; //if code gets here, ID was not found
}

void ThreadStore::activateDeadThread(TCB* deadThread){

	TCB* foundThread = NULL;
	//create a context for the thread

	for(std::list<TCB*>::iterator TCBItr = deadList->begin(); TCBItr != deadList->end(); TCBItr++){
	
		if(*TCBItr == deadThread){	//if found TCB matches given TCB
			foundThread = *TCBItr;
			break;
		}
	}
	if(foundThread == deadThread){
		TVMThreadPriority priority = foundThread->getPriority();	//get priority of found thread
		foundThread->setState(VM_THREAD_STATE_READY);							//set thread state to READY
		readyLists[priority]->push_back(foundThread);								//add to back of ready list - make this front?
		if(foundThread->getPriority() > currentThread->getPriority()){
			schedule(VM_THREAD_PRIORITY_HIGH);
		}
	}
}

void ThreadStore::terminate(TCB* thread){

//	printf("terminate(): TID %d terminating TID %d\n", currentThread->getThreadID(), thread->getThreadID());

	if(thread->getEntryFunction() != &idle){
		thread->releaseMutexes();
		thread->setState(VM_THREAD_STATE_DEAD);	//mark thread as dead
		deadList->push_back(thread);						//push to dead list
		schedule(VM_THREAD_PRIORITY_HIGH);
	}
	return;
}

void ThreadStore::terminateCurrentThread(){
	terminate(currentThread);
}


void ThreadStore::switchToNewThread(TCB* nextThread){

	nextThread->setState(VM_THREAD_STATE_RUNNING);		//set next thread to running
	//if currentThread dead or idle, DON'T push to wait list!	

	if(currentThread->getEntryFunction() != &idle){	//if current thread is idle, skip

		if(currentThread->getState() == VM_THREAD_STATE_WAITING){
			waitList->push_back(currentThread);	//add current thread - one to be swapped out - to wait list
		}
		else if(currentThread->getState() == VM_THREAD_STATE_DEAD){	//activating a dead thread
		}
		else if(currentThread->getState() == VM_THREAD_STATE_READY){	//activating a dead thread
			readyLists[currentThread->getPriority()]->push_back(currentThread);	//go back in ready queue
		}
		else if(currentThread->getState() == VM_THREAD_STATE_RUNNING){
			currentThread->setState(VM_THREAD_STATE_READY);	//put state to ready
			readyLists[currentThread->getPriority()]->push_back(currentThread);	//go back in ready queue
		}
	}

	SMachineContextRef oldContext = currentThread->contextRef;	//old context is current thread's
	SMachineContextRef newContext = nextThread->contextRef;			//new context is next thread's
	currentThread = nextThread;																	//set current active thread to be next thread
	MachineContextSwitch(oldContext, newContext);
	return;
}

void ThreadStore::schedule(TVMThreadPriority priority){ //priority = thread priority to schedule first. default is HIGH.

	//Checks if there are any threads ready to run in any ready lists
	//If no threads, runs idle thread until timerEvent is called at next timeslice

	bool areAllListsEmpty = (readyLists[0x01]->empty() && readyLists[0x02]->empty() && readyLists[0x03]->empty());

	if((areAllListsEmpty == true) && (currentThread->getState() == VM_THREAD_STATE_RUNNING)){
//		printf("schedule(): all ready lists empty, returning\n");
		return;
	}

	if(readyLists[priority]->empty() == false){	//attempt to swap in thread of given priority
		TCB* thread = readyLists[priority]->front();
		if(priority != 0x00){												//if list is not special idle thread queue
			readyLists[priority]->pop_front();				//pop old pointer from the list
		}
//		printf("schedule(): switching to TID %d of priority %d", thread->getThreadID(), thread->getPriority());
		switchToNewThread(thread);	//switch to new thread of priority priority
	}
	else{
		TCB* newThread;																		//no threads of given priority exist
		for(TVMThreadPriority i = 0x03; i >= 0x00; i--){	//Check highest prio and work down
			if(i == priority){										//skip priority level that was already checked
				continue;
			}
			if(readyLists[i]->empty() == false){ 		//Queue is not empty
				newThread = readyLists[i]->front();
				if(i != 0x0){												//if list is not special idle thread queue
					readyLists[i]->pop_front();				//pop old pointer from the list
				}
				break;
			}
		}
//		printf("schedule(): switching to TID %d of priority %d\n", newThread->getThreadID(),newThread->getPriority());
		switchToNewThread(newThread);
	}
}

void ThreadStore::timerEvent(){
	//checks wait list for any threads that have finished sleeping
	//moves threads that are done sleeping from wait list into appropriate ready list
	//ThreadStore::schedule() is then called, which swaps out the current thread for a thread from a ready list

	for(std::list<TCB*>::iterator i = waitList->begin(); i != waitList->end(); i++){	//iterate over entire wait list
		if(((*i)->isWaitingOnMutex() == true) && ((*i)->getWaitingMutex() != NULL)){	//if waiting on AT LEAST ONE mutex
			if((*i) == (*i)->getWaitingMutex()->getOwner()){	//if mutex was acquired, ie if mutex owner is same as waiting thread
				//waitingMutex is necessarily the mutex thread i most recently tried to acquire
				(*i)->setWaitingOnMutex(false);		//no longer waiting
				(*i)->sleep(0);		//set ticks to sleep = 0
			}
		}
		if((*i)->isWaitingOnIO() == true){	//if the thread is waiting on IO, ignore it, even if it has mutex
			continue;
		}
		if(((*i)->getTicksToSleep() != (unsigned)(-1)) && ((*i)->getTicksToSleep() > 0)){	//-1 means it will sleep infinitely
				//if thread still has ticks to sleep and is not sleeping infinitely, decrement ticks by 1
			(*i)->decrementTicksToSleep();
		}
		else if((*i)->getTicksToSleep() == (unsigned)(-1)){	//-1 means it will sleep infinitely
			continue;
		} 
		else if((*i)->getTicksToSleep() == 0){	//if thread has no ticks to sleep, put in ready list, even if no mutex
			(*i)->setState(VM_THREAD_STATE_READY);
			readyLists[(*i)->getPriority()]->push_back(*i);			//insert thread into ready list
			waitList->erase(i);															//remove thread from wait list
			i--;
		}
	}

	//if all ready lists are empty, the wait list is empty, and idle thread is running, call MachineTerminate() 
	if((readyLists[0x01]->empty() && readyLists[0x02]->empty() && readyLists[0x03]->empty())){
		if(waitList->empty() && (currentThread->getEntryFunction() == &idle)){
		//	printf("terminating because nothing to do\n");
			activateDeadThread(findThreadByID(0));
		}
	}
	schedule(VM_THREAD_PRIORITY_HIGH);
}

ThreadStore *ThreadStore::getInstance(){
    if(NULL == DUniqueInstance){
        DUniqueInstance = new ThreadStore();
    }
    return DUniqueInstance;
}

void ThreadStore::createSystemMemoryPool(TVMMemorySize heapSize){
	uint8_t* systemMemoryBaseAddress = new uint8_t[heapSize];
	MemoryPool* systemMemoryPool = new MemoryPool(systemMemoryBaseAddress, heapSize);
	memoryPoolVector->push_back(systemMemoryPool);
	numMemoryPools++;
}

void ThreadStore::createSharedMemoryPool(uint8_t *sharedMemoryBaseAddress, TVMMemorySize sharedMemorySize){
	MemoryPool* sharedMemoryPool = 	new MemoryPool(sharedMemoryBaseAddress, sharedMemorySize);
	memoryPoolVector->push_back(sharedMemoryPool);
	numMemoryPools++;
}

ThreadStore::ThreadStore(){
	
	for(TVMThreadPriority i = 0x00; i < 0x04; i++){
		//priority lists: 0x0 is idle, 0x1 is low, 0x2 is mid, 0x3 is high
		readyLists[i] = new std::list<TCB*>();	
	}

	for(TVMThreadPriority i = 0x00; i <= 0x02; i++){
		memoryWaitingLists[i] = new std::list<MemoryWaitingInfo*>();
	}

	allThreads = new std::vector<TCB*>();
	waitList = new std::list<TCB*>();			//the list of threads in waiting state
	deadList = new std::list<TCB*>();			//the list of threads in dead state
	mutexVector = new std::vector<Mutex*>();	//vector of all mutexes
	memoryPoolVector = new std::vector<MemoryPool*>();	//vector of all mutexes
	numThreads = -1;
	numMutexes = -1;
	numMemoryPools = -1;
	voidParam = 0;
	idleThreadID = 0;		//initialize the idle thread's id to zero, it will be overwritten
}

TCB* ThreadStore::getCurrentThread(){
	return currentThread;
}

bool ThreadStore::isThreadPrioInRange(TCB* tcb){

	//if the thread's priority is less than 1 or greater than 3
	if((tcb->getPriority() < (TVMThreadPriority)0x01) || (tcb->getPriority() > (TVMThreadPriority)0x03)){
		return false;	//false = we are out of range = bad
	}
	else
		return true;	//we are not out of range so we are in range
}

void ThreadStore::insert(TCB* tcb){	
//insert a newly created thread into the correct container
//currently, only the dead case should be called

	TVMThreadPriority priority = tcb->getPriority();
	TVMThreadState state = tcb->getState();
	allThreads->push_back(tcb);
	
	switch(state){
		
		case VM_THREAD_STATE_DEAD:
			deadList->push_back(tcb);
			break;
		case VM_THREAD_STATE_WAITING:
			waitList->push_back(tcb);
		case VM_THREAD_STATE_READY:
			readyLists[priority]->push_back(tcb);			//otherwise go ahead and add it
			//lists[0x01] is low, queues[0x02] is mid, queues[0x03] is high
			break;
		default:
			break;
	}
	numThreads++;	//increment number of threads
}

void ThreadStore::insert(Mutex *mutex){
//adds a mutex to the vector of mutexes
	mutexVector->push_back(mutex);
	numMutexes++;
}

void ThreadStore::insert(MemoryPool *memoryPool){
//adds a memory pool to the vector of mutexes
	memoryPoolVector->push_back(memoryPool);
	numMemoryPools = numMemoryPools + 1;
}
void ThreadStore::createMainThread(){

	//initialize parameters for creating vmStart thread
	TVMThreadEntry vmStartEntry = (TVMThreadEntry)&VMStart;
	int vmStartParam = 0;
	TVMMemorySize vmStartMemsize = 0;	//arbitrary
	TVMThreadPriority vmStartPrio = VM_THREAD_PRIORITY_NORMAL;
	TVMThreadID vmStartTID = 0xFFFFFFFF;	//will be overwritten with 1
	TCB* vmStartThread = new TCB(vmStartEntry, (void*)&vmStartParam, vmStartMemsize, vmStartPrio, (TVMThreadIDRef)&vmStartTID);
	insert(vmStartThread);
	currentThread = vmStartThread;
}

void ThreadStore::createIdleThread(){

	TCB* idleThread;

	if(readyLists[0x0]->empty() == true){	//if no idle thread exists
		TVMMemorySize m = 0x100000;
		TVMThreadPriority p = 0x00;
		//idle thread has its own special list
		idleThread = new TCB((TVMThreadEntry)&idle, (void *)(&voidParam), m, p, &idleThreadID);
		readyLists[0x0]->push_back(idleThread);
		allThreads->push_back(idleThread);
		numThreads++;
	}
}

TVMMemoryPoolID ThreadStore::getNumMemoryPools(){
	return numMemoryPools;
}

int ThreadStore::getNumThreads(){
	return numThreads;
}

ThreadStore::~ThreadStore(){

	//delete the memory pools BEFORE deleting the threads
	//because the threads' stacks are allocated from the memory pools,
	//DO NOT attempt to delete the STACKS! The stacks will go away automatically
	//when the parent memory pool is deleted. This means the TCB destructor must
	//be modified to NOT attempt to delete the stacks.
	
	while(allThreads->empty() == false){
		TCB* thread = allThreads->back();
		if(thread != NULL)
			delete thread;
		allThreads->pop_back();
	}
	delete allThreads;

	while(memoryPoolVector->empty() == false){
		MemoryPool *pool = memoryPoolVector->back();
		memoryPoolVector->pop_back();
		if(pool != NULL){
			delete pool;
		}
	}
	delete systemMemoryBaseAddress;

	while(mutexVector->empty() == false){
		Mutex *mutex = mutexVector->back();
		mutexVector->pop_back();
		delete mutex;
	}
	delete mutexVector;	

	for(TVMThreadPriority i = 0x00; i < 0x04; i++){	//iterate over all ready lists
		while(readyLists[i]->empty() == false){				//while the list is not empty
			readyLists[i]->pop_front();									//pop off head of list
		}
		delete readyLists[i];													//delete empty container
	}

	while(waitList->empty() == false){
		waitList->pop_front();						//erase head of list
	}
	delete waitList;

	while(deadList->empty() == false){
		deadList->pop_front();						//erase head of list
	}
	delete deadList;
}

//=================================================================



//=== Utilities ==================================================

void safeEntry(void *functionAndParams){
	//this is the skeleton function
	MachineEnableSignals();

	ThreadStore *tStore = ThreadStore::getInstance();
//	printf("safeEntry(): entering thread TID %d\n", tStore->getCurrentThread()->getThreadID());
	TVMThreadEntry entry 	= (TVMThreadEntry)(((int**)functionAndParams)[0]);
	void *params 					= (void *)(((int**)functionAndParams)[1]);
	entry(params);				//this starts the thread
//	printf("safeEntry(): returned, terminating TID %d\n", tStore->getCurrentThread()->getThreadID());
//	printf("safeEntry(): about to terminate TID %d\n", tStore->getCurrentThread()->getThreadID());
	tStore->terminateCurrentThread();
	//this is called if user does not terminate on their own
}

void idle(void* parm1){	//infinite loop for the idle thread
	MachineEnableSignals();
	while(1);
}

TVMStatus successfulExit(){
	return VM_STATUS_SUCCESS;
}

void timerInterrupt(void *calldata){
	TMachineSignalState sigState;
	MachineSuspendSignals(&sigState);		
	ThreadStore::getInstance()->timerEvent();	//scheduling logic is in the threadstore because that's where all the TCBs are
	MachineResumeSignals(&sigState);		//resume signals once scheduling ops are done
	return;
}

void machineFileIOCallback(void* calledParam, int result){

	((TCB*)calledParam)->setFileIOResult(result);	//return file descriptor to original location by reference. will be -1 if an error occurs
	((TCB*)calledParam)->setIsWaitingOnIO(false);
	return;
}
//==================================================================
